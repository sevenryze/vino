/*
 * rfbserver.c - deal with server-side of the RFB protocol.
 */

/*
 *  Copyright (C) 2002 RealVNC Ltd.
 *  OSXvnc Copyright (C) 2001 Dan McGuirk <mcguirk@incompleteness.net>.
 *  Original Xvnc code Copyright (C) 1999 AT&T Laboratories Cambridge.  
 *  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

#define _GNU_SOURCE 1

#include <string.h>
#include "rfb/rfb.h"
#include "rfb/rfbregion.h"
#include <netdb.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef WIN32
#define write(sock,buf,len) send(sock,buf,len,0)
#else
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <pwd.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif
#endif

#ifdef DEBUGPROTO
#undef DEBUGPROTO
#define DEBUGPROTO(x) x
#else
#define DEBUGPROTO(x)
#endif

rfbClientPtr pointerClient = NULL;  /* Mutex for pointer events */

static void rfbProcessClientProtocolVersion(rfbClientPtr cl);
static void rfbProcessClientNormalMessage(rfbClientPtr cl);

struct rfbClientIterator {
  rfbClientPtr next;
  rfbScreenInfoPtr screen;
};

void
rfbClientListInit(rfbScreenInfoPtr rfbScreen)
{
    if(sizeof(rfbBool)!=1) {
        /* a sanity check */
        fprintf(stderr,"rfbBool's size is not 1 (%lu)!\n",(unsigned long)sizeof(rfbBool));
	/* we cannot continue, because rfbBool is supposed to be char everywhere */
	exit(1);
    }
    rfbScreen->rfbClientHead = NULL;
}

rfbClientIteratorPtr
rfbGetClientIterator(rfbScreenInfoPtr rfbScreen)
{
  rfbClientIteratorPtr i =
    (rfbClientIteratorPtr)malloc(sizeof(struct rfbClientIterator));
  i->next = 0;
  i->screen = rfbScreen;
  return i;
}

rfbClientPtr
rfbClientIteratorHead(rfbClientIteratorPtr i)
{
  LOCK(rfbClientListMutex);
  i->next = i->screen->rfbClientHead;
  UNLOCK(rfbClientListMutex);
  return i->next;
}

rfbClientPtr
rfbClientIteratorNext(rfbClientIteratorPtr i)
{
  if(i->next == 0) {
    LOCK(rfbClientListMutex);
    i->next = i->screen->rfbClientHead;
    UNLOCK(rfbClientListMutex);
  } else {
    i->next = i->next->next;
  }

    return i->next;
}

void
rfbReleaseClientIterator(rfbClientIteratorPtr iterator)
{
  free(iterator);
}

/*
 * rfbNewClientConnection is called from sockets.c when a new connection
 * comes in.
 */

void
rfbNewClientConnection(rfbScreenInfoPtr rfbScreen,
                       int sock)
{
    rfbClientPtr cl;

    cl = rfbNewClient(rfbScreen,sock);
}


/*
 * rfbNewClient is called when a new connection has been made by whatever
 * means.
 */

rfbClientPtr
rfbNewClient(rfbScreenInfoPtr rfbScreen,
             int sock)
{
    rfbProtocolVersionMsg pv;
    rfbClientIteratorPtr iterator;
    rfbClientPtr cl,cl_;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    int i;
    char host[NI_MAXHOST] = "(unresolved)";
    const char *prt = "unknown";

    cl = (rfbClientPtr)calloc(sizeof(rfbClientRec),1);

    cl->screen = rfbScreen;
    cl->sock = sock;
    cl->viewOnly = FALSE;

    rfbResetStats(cl);

    {
      int one=1;

      getpeername(sock, (struct sockaddr *)&addr, &addrlen);

      if (getnameinfo((struct sockaddr *)&addr, addrlen,
                      host, sizeof(host), NULL, 0, 0)) {
        getnameinfo((struct sockaddr *)&addr, addrlen,
                    host, sizeof(host), NULL, 0, NI_NUMERICHOST);
      }

      cl->host = strdup(host);

      if (addr.ss_family == AF_INET)
        prt = "IPv4";
      else if (addr.ss_family == AF_INET6)
        prt = "IPv6";

      rfbLog("[%s] Got connection from client %s\n", prt, cl->host);

      rfbLog("  other clients:\n");
      iterator = rfbGetClientIterator(rfbScreen);
      while ((cl_ = rfbClientIteratorNext(iterator)) != NULL) {
        rfbLog("     %s\n",cl_->host);
      }
      rfbReleaseClientIterator(iterator);

#ifndef WIN32
      if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
	rfbLogPerror("fcntl failed");
	close(sock);
	free(cl);
	return NULL;
      }
#endif

      if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
		     (char *)&one, sizeof(one)) < 0) {
	rfbLogPerror("setsockopt failed");
	close(sock);
	free(cl);
	return NULL;
      }

      FD_SET(sock,&(rfbScreen->allFds));
		rfbScreen->maxFd = max(sock,rfbScreen->maxFd);

      cl->state = RFB_PROTOCOL_VERSION;

      cl->readyForSetColourMapEntries = FALSE;
      cl->useCopyRect = FALSE;
      cl->preferredEncoding = rfbEncodingRaw;
      cl->correMaxWidth = 48;
      cl->correMaxHeight = 48;
#ifdef VINO_HAVE_ZLIB
      cl->zrleData = NULL;
#endif

      cl->copyRegion = sraRgnCreate();
      cl->copyDX = 0;
      cl->copyDY = 0;
   
      cl->modifiedRegion =
	sraRgnCreateRect(0,0,rfbScreen->width,rfbScreen->height);

      cl->requestedRegion = sraRgnCreate();

      cl->format = cl->screen->rfbServerFormat;
      cl->translateFn = rfbTranslateNone;
      cl->translateLookupTable = NULL;

      LOCK(rfbClientListMutex);

      cl->next = rfbScreen->rfbClientHead;
      cl->prev = NULL;
      if (rfbScreen->rfbClientHead)
        rfbScreen->rfbClientHead->prev = cl;

      rfbScreen->rfbClientHead = cl;
      UNLOCK(rfbClientListMutex);

#ifdef VINO_HAVE_JPEG
      cl->tightCompressLevel = TIGHT_DEFAULT_COMPRESSION;
      cl->tightQualityLevel = -1;
      for (i = 0; i < 4; i++)
        cl->zsActive[i] = FALSE;
#endif

      cl->enableCursorUpdates = FALSE;
      cl->useRichCursorEncoding = FALSE;
      cl->enableLastRectEncoding = FALSE;
      cl->useNewFBSize = FALSE;

#ifdef VINO_HAVE_ZLIB
      cl->compStreamInited = FALSE;
      cl->compStream.total_in = 0;
      cl->compStream.total_out = 0;
      cl->compStream.zalloc = Z_NULL;
      cl->compStream.zfree = Z_NULL;
      cl->compStream.opaque = Z_NULL;

      cl->zlibCompressLevel = 5;
#endif

      sprintf(pv,rfbProtocolVersionFormat,rfbProtocolMajorVersion,
	      rfbProtocolMinorVersion7);

      if (WriteExact(cl, pv, sz_rfbProtocolVersionMsg) < 0) {
        rfbLogPerror("rfbNewClient: write");
        rfbCloseClient(cl);
        rfbClientConnectionGone(cl);
        return NULL;
      }
    }

    cl->clientData = NULL;
    cl->clientGoneHook = NULL;
    cl->onHold = FALSE;
    if (cl->screen->newClientHook) {
	switch (cl->screen->newClientHook(cl)) {
	case RFB_CLIENT_ON_HOLD:
	    cl->onHold = TRUE;
	    break;
	case RFB_CLIENT_ACCEPT:
	    break;
	case RFB_CLIENT_REFUSE:
	    rfbCloseClient(cl);
	    rfbClientConnectionGone(cl);
	    cl = NULL;
	    break;
	}
    }

    return cl;
}

/*
 * rfbClientConnectionGone is called from sockets.c just after a connection
 * has gone away.
 */

void
rfbClientConnectionGone(rfbClientPtr cl)
{
    int i;

    LOCK(rfbClientListMutex);

    if (cl->prev)
        cl->prev->next = cl->next;
    else
        cl->screen->rfbClientHead = cl->next;
    if (cl->next)
        cl->next->prev = cl->prev;

    if(cl->sock>0)
        close(cl->sock);

#ifdef VINO_HAVE_ZLIB
    FreeZrleData(cl);
#endif

    if(cl->sock>=0)
       FD_CLR(cl->sock,&(cl->screen->allFds));

    if (cl->clientGoneHook)
      cl->clientGoneHook(cl);

    rfbLog("Client %s gone\n",cl->host);
    free(cl->host);

#ifdef VINO_HAVE_ZLIB
    /* Release the compression state structures if any. */
    if ( cl->compStreamInited ) {
	deflateEnd( &(cl->compStream) );
    }

#ifdef VINO_HAVE_JPEG
    for (i = 0; i < 4; i++) {
	if (cl->zsActive[i])
	    deflateEnd(&cl->zsStruct[i]);
    }
#endif
#endif

    if (pointerClient == cl)
        pointerClient = NULL;

    sraRgnDestroy(cl->modifiedRegion);
    sraRgnDestroy(cl->requestedRegion);
    sraRgnDestroy(cl->copyRegion);

    UNLOCK(rfbClientListMutex);

    if (cl->translateLookupTable) free(cl->translateLookupTable);

    rfbPrintStats(cl);

    free(cl);
}


/*
 * rfbProcessClientMessage is called when there is data to read from a client.
 */

void
rfbProcessClientMessage(rfbClientPtr cl)
{
    switch (cl->state) {
    case RFB_PROTOCOL_VERSION:
        rfbProcessClientProtocolVersion(cl);
        return;
    case RFB_SECURITY_TYPE:
	rfbAuthProcessSecurityTypeMessage(cl);
	return;
#ifdef VINO_HAVE_GNUTLS
    case RFB_TLS_HANDSHAKE:
	rfbAuthProcessTLSHandshake(cl);
	return;
#endif
    case RFB_AUTH_TYPE:
	rfbAuthProcessAuthTypeMessage(cl);
	return;
    case RFB_AUTHENTICATION:
        rfbAuthProcessClientMessage(cl);
        return;
    case RFB_AUTH_DEFERRED:
	rfbLog("Authentication deferred for this client - closing connection\n");
	rfbCloseClient(cl);
	return;
    case RFB_INITIALISATION:
        rfbProcessClientInitMessage(cl);
        return;
    default:
        rfbProcessClientNormalMessage(cl);
        return;
    }
}


/*
 * rfbProcessClientProtocolVersion is called when the client sends its
 * protocol version.
 */

static void
rfbProcessClientProtocolVersion(rfbClientPtr cl)
{
    rfbProtocolVersionMsg pv;
    int n, major_, minor_;

    if ((n = ReadExact(cl, pv, sz_rfbProtocolVersionMsg)) <= 0) {
        if (n == 0)
            rfbLog("rfbProcessClientProtocolVersion: client gone\n");
        else
            rfbLogPerror("rfbProcessClientProtocolVersion: read");
        rfbCloseClient(cl);
        return;
    }

    pv[sz_rfbProtocolVersionMsg] = 0;
    if (sscanf(pv,rfbProtocolVersionFormat,&major_,&minor_) != 2) {
        char name[1024]; 
	if(sscanf(pv,"RFB %03d.%03d %1023s\n",&major_,&minor_,name) != 3) {
	    rfbErr("rfbProcessClientProtocolVersion: not a valid RFB client\n");
	    rfbCloseClient(cl);
	    return;
	}
	free(cl->host);
	cl->host=strdup(name);
    }
    rfbLog("Client Protocol Version %d.%d\n", major_, minor_);

    if (major_ != rfbProtocolMajorVersion) {
        rfbErr("RFB protocol version mismatch - server %d.%d, client %d.%d",
                rfbProtocolMajorVersion, rfbProtocolMinorVersion7,
                major_,minor_);
        rfbCloseClient(cl);
        return;
    }

    if (minor_ != rfbProtocolMinorVersion3 &&
	minor_ != rfbProtocolMinorVersion7 &&
	minor_ != rfbProtocolMinorVersion8) {
        /* Minor version mismatch - warn but try to continue */
        rfbLog("Ignoring minor version mismatch\n");
    }

    if (minor_ >= rfbProtocolMinorVersion8)
      cl->minorVersion = rfbProtocolMinorVersion8;
    else if (minor_ == rfbProtocolMinorVersion7)
      cl->minorVersion = rfbProtocolMinorVersion7;
    else
      cl->minorVersion = rfbProtocolMinorVersion3;

    rfbAuthNewClient(cl);
}


/*
 * rfbClientConnFailed is called when a client connection has failed either
 * because it talks the wrong protocol or it has failed authentication.
 */

void
rfbClientConnFailed(rfbClientPtr cl,
                    char *reason)
{
    char *buf;
    int len = strlen(reason);

    rfbLog("rfbClientConnFailed(\"%s\")\n", reason);

    buf = (char *)malloc(8 + len);
    ((uint32_t *)buf)[0] = 0;
    ((uint32_t *)buf)[1] = Swap32IfLE(len);
    memcpy(buf + 8, reason, len);

    if (WriteExact(cl, buf, 8 + len) < 0)
        rfbLogPerror("rfbClientConnFailed: write");
    free(buf);

    rfbCloseClient(cl);
}


/*
 * rfbProcessClientInitMessage is called when the client sends its
 * initialisation message.
 */

void
rfbProcessClientInitMessage(rfbClientPtr cl)
{
    rfbClientInitMsg ci;
    char buf[256];
    rfbServerInitMsg *si = (rfbServerInitMsg *)buf;
    int len, n;
    rfbClientIteratorPtr iterator;
    rfbClientPtr otherCl;

    if ((n = ReadExact(cl, (char *)&ci,sz_rfbClientInitMsg)) <= 0) {
        if (n == 0)
            rfbLog("rfbProcessClientInitMessage: client gone\n");
        else
            rfbLogPerror("rfbProcessClientInitMessage: read");
        rfbCloseClient(cl);
        return;
    }

    memset(buf,0,sizeof(buf));

    si->framebufferWidth = Swap16IfLE(cl->screen->width);
    si->framebufferHeight = Swap16IfLE(cl->screen->height);
    si->format = cl->screen->rfbServerFormat;
    si->format.redMax = Swap16IfLE(si->format.redMax);
    si->format.greenMax = Swap16IfLE(si->format.greenMax);
    si->format.blueMax = Swap16IfLE(si->format.blueMax);

    strncpy(buf + sz_rfbServerInitMsg, cl->screen->desktopName, 127);
    len = strlen(buf + sz_rfbServerInitMsg);
    si->nameLength = Swap32IfLE(len);

    if (WriteExact(cl, buf, sz_rfbServerInitMsg + len) < 0) {
        rfbLogPerror("rfbProcessClientInitMessage: write");
        rfbCloseClient(cl);
        return;
    }

    cl->state = RFB_NORMAL;

    if (cl->screen->rfbNeverShared || (!cl->screen->rfbAlwaysShared && !ci.shared)) {

        if (cl->screen->rfbDontDisconnect) {
            iterator = rfbGetClientIterator(cl->screen);
            while ((otherCl = rfbClientIteratorNext(iterator)) != NULL) {
                if ((otherCl != cl) && (otherCl->state == RFB_NORMAL)) {
                    rfbLog("-dontdisconnect: Not shared & existing client\n");
                    rfbLog("  refusing new client %s\n", cl->host);
                    rfbCloseClient(cl);
                    rfbReleaseClientIterator(iterator);
                    return;
                }
            }
            rfbReleaseClientIterator(iterator);
        } else {
            iterator = rfbGetClientIterator(cl->screen);
            while ((otherCl = rfbClientIteratorNext(iterator)) != NULL) {
                if ((otherCl != cl) && (otherCl->state == RFB_NORMAL)) {
                    rfbLog("Not shared - closing connection to client %s\n",
                           otherCl->host);
                    rfbCloseClient(otherCl);
                }
            }
            rfbReleaseClientIterator(iterator);
        }
    }
}


/*
 * rfbProcessClientNormalMessage is called when the client has sent a normal
 * protocol message.
 */

static void
rfbProcessClientNormalMessage(rfbClientPtr cl)
{
    int n=0;
    rfbClientToServerMsg msg;
    char *str;

    if ((n = ReadExact(cl, (char *)&msg, 1)) <= 0) {
        if (n != 0)
            rfbLogPerror("rfbProcessClientNormalMessage: read");
        rfbCloseClient(cl);
        return;
    }

    switch (msg.type) {

    case rfbSetPixelFormat:

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbSetPixelFormatMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        cl->format.bitsPerPixel = msg.spf.format.bitsPerPixel;
        cl->format.depth = msg.spf.format.depth;
        cl->format.bigEndian = (msg.spf.format.bigEndian ? TRUE : FALSE);
        cl->format.trueColour = (msg.spf.format.trueColour ? TRUE : FALSE);
        cl->format.redMax = Swap16IfLE(msg.spf.format.redMax);
        cl->format.greenMax = Swap16IfLE(msg.spf.format.greenMax);
        cl->format.blueMax = Swap16IfLE(msg.spf.format.blueMax);
        cl->format.redShift = msg.spf.format.redShift;
        cl->format.greenShift = msg.spf.format.greenShift;
        cl->format.blueShift = msg.spf.format.blueShift;

	cl->readyForSetColourMapEntries = TRUE;

        rfbSetTranslateFunction(cl);

        return;


    case rfbFixColourMapEntries:
        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbFixColourMapEntriesMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }
        rfbLog("rfbProcessClientNormalMessage: %s",
                "FixColourMapEntries unsupported\n");
        rfbCloseClient(cl);
        return;


    case rfbSetEncodings:
    {
        int i;
        uint32_t enc;
	rfbBool enableCursorShapeUpdates, enableCursorPosUpdates;
	rfbBool useRichCursorEncoding;

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbSetEncodingsMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        msg.se.nEncodings = Swap16IfLE(msg.se.nEncodings);

	enableCursorShapeUpdates = FALSE;
	enableCursorPosUpdates = FALSE;
	useRichCursorEncoding = FALSE;

        cl->preferredEncoding = -1;
	cl->useCopyRect = FALSE;
	cl->enableLastRectEncoding = FALSE;
        cl->useNewFBSize = FALSE;
#ifdef VINO_HAVE_JPEG
        cl->tightQualityLevel = -1;
#endif

        for (i = 0; i < msg.se.nEncodings; i++) {
            if ((n = ReadExact(cl, (char *)&enc, 4)) <= 0) {
                if (n != 0)
                    rfbLogPerror("rfbProcessClientNormalMessage: read");
                rfbCloseClient(cl);
                return;
            }
            enc = Swap32IfLE(enc);

            switch (enc) {

            case rfbEncodingCopyRect:
		cl->useCopyRect = TRUE;
                break;
            case rfbEncodingRaw:
            case rfbEncodingRRE:
            case rfbEncodingCoRRE:
            case rfbEncodingHextile:
#ifdef VINO_HAVE_ZLIB
	    case rfbEncodingZlib:
            case rfbEncodingZRLE:
#ifdef VINO_HAVE_JPEG
	    case rfbEncodingTight:
#endif
#endif
            /* The first supported encoding is the 'preferred' encoding */
                if (cl->preferredEncoding == -1)
                    cl->preferredEncoding = enc;


                break;
	    case rfbEncodingXCursor:
		enableCursorShapeUpdates = TRUE;
		break;
	    case rfbEncodingRichCursor:
	        enableCursorShapeUpdates = TRUE;
	        useRichCursorEncoding = TRUE;
	        break;
	    case rfbEncodingPointerPos:
		enableCursorPosUpdates = TRUE;
	        break;
	    case rfbEncodingLastRect:
		if (!cl->enableLastRectEncoding) {
		    rfbLog("Enabling LastRect protocol extension for client "
			   "%s\n", cl->host);
		    cl->enableLastRectEncoding = TRUE;
		}
		break;
	    case rfbEncodingNewFBSize:
		if (!cl->useNewFBSize) {
		    rfbLog("Enabling NewFBSize protocol extension for client "
			   "%s\n", cl->host);
		    cl->useNewFBSize = TRUE;
		}
		break;
            default:
#ifdef VINO_HAVE_ZLIB
		if ( enc >= (uint32_t)rfbEncodingCompressLevel0 &&
		     enc <= (uint32_t)rfbEncodingCompressLevel9 ) {
		    cl->zlibCompressLevel = enc & 0x0F;
#ifdef VINO_HAVE_JPEG
		    cl->tightCompressLevel = enc & 0x0F;
		    rfbLog("Using compression level %d for client %s\n",
			   cl->tightCompressLevel, cl->host);
		} else if ( enc >= (uint32_t)rfbEncodingQualityLevel0 &&
			    enc <= (uint32_t)rfbEncodingQualityLevel9 ) {
		    cl->tightQualityLevel = enc & 0x0F;
		    rfbLog("Using image quality level %d for client %s\n",
			   cl->tightQualityLevel, cl->host);
#endif
		} else
#endif
		 rfbLog("rfbProcessClientNormalMessage: ignoring unknown "
                       "encoding type %d\n", (int)enc);
            }
        }

        if (cl->preferredEncoding == -1) {
            cl->preferredEncoding = rfbEncodingRaw;
        }

	if (enableCursorPosUpdates && enableCursorShapeUpdates) {
	  rfbLog("Enabling cursor position and shape (%s encoding) updates for client %s\n",
		 useRichCursorEncoding ? "rich" : "X", cl->host);
	  cl->enableCursorUpdates = TRUE;
	  cl->useRichCursorEncoding = useRichCursorEncoding;
	} else {
	  cl->enableCursorUpdates = FALSE;
	  cl->useRichCursorEncoding = FALSE;
	}

        return;
    }


    case rfbFramebufferUpdateRequest:
    {
        sraRegionPtr tmpRegion;

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbFramebufferUpdateRequestMsg-1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

	tmpRegion =
	  sraRgnCreateRect(Swap16IfLE(msg.fur.x),
			   Swap16IfLE(msg.fur.y),
			   Swap16IfLE(msg.fur.x)+Swap16IfLE(msg.fur.w),
			   Swap16IfLE(msg.fur.y)+Swap16IfLE(msg.fur.h));

        LOCK(cl->updateMutex);
	sraRgnOr(cl->requestedRegion,tmpRegion);

	if (!cl->readyForSetColourMapEntries) {
	    /* client hasn't sent a SetPixelFormat so is using server's */
	    cl->readyForSetColourMapEntries = TRUE;
	    if (!cl->format.trueColour) {
		if (!rfbSetClientColourMap(cl, 0, 0)) {
		    sraRgnDestroy(tmpRegion);
		    UNLOCK(cl->updateMutex);
		    return;
		}
	    }
	}

       if (!msg.fur.incremental) {
	    sraRgnOr(cl->modifiedRegion,tmpRegion);
	    sraRgnSubtract(cl->copyRegion,tmpRegion);
       }
       UNLOCK(cl->updateMutex);

       sraRgnDestroy(tmpRegion);

       return;
    }

    case rfbKeyEvent:

	cl->rfbKeyEventsRcvd++;

	if ((n = ReadExact(cl, ((char *)&msg) + 1,
			   sz_rfbKeyEventMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseClient(cl);
	    return;
	}

	if(!cl->viewOnly && cl->screen->kbdAddEvent) {
	    cl->screen->kbdAddEvent(msg.ke.down, (rfbKeySym)Swap32IfLE(msg.ke.key), cl);
	}

        return;


    case rfbPointerEvent:

	cl->rfbPointerEventsRcvd++;

	if ((n = ReadExact(cl, ((char *)&msg) + 1,
			   sz_rfbPointerEventMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseClient(cl);
	    return;
	}

	if (pointerClient && (pointerClient != cl))
	    return;

	if (msg.pe.buttonMask == 0)
	    pointerClient = NULL;
	else
	    pointerClient = cl;

	if(!cl->viewOnly) {
	    cl->screen->ptrAddEvent(msg.pe.buttonMask,
				    Swap16IfLE(msg.pe.x), Swap16IfLE(msg.pe.y), cl);
	}

        return;


    case rfbClientCutText:

	if ((n = ReadExact(cl, ((char *)&msg) + 1,
			   sz_rfbClientCutTextMsg - 1)) <= 0) {
	    if (n != 0)
		rfbLogPerror("rfbProcessClientNormalMessage: read");
	    rfbCloseClient(cl);
	    return;
	}

	msg.cct.length = Swap32IfLE(msg.cct.length);

	str = (char *)malloc(msg.cct.length);

	if ((n = ReadExact(cl, str, msg.cct.length)) <= 0) {
	    if (n != 0)
	        rfbLogPerror("rfbProcessClientNormalMessage: read");
	    free(str);
	    rfbCloseClient(cl);
	    return;
	}

	if(!cl->viewOnly) {
	    if (cl->screen->setXCutText)
	      cl->screen->setXCutText(str, msg.cct.length, cl);
	}
	free(str);

        return;


    default:

        rfbLog("rfbProcessClientNormalMessage: unknown message type %d\n",
                msg.type);
        rfbLog(" ... closing connection\n");
        rfbCloseClient(cl);
        return;
    }
}



/*
 * rfbSendFramebufferUpdate - send the currently pending framebuffer update to
 * the RFB client.
 * givenUpdateRegion is not changed.
 */

rfbBool
rfbSendFramebufferUpdate(rfbClientPtr cl,
                         sraRegionPtr givenUpdateRegion)
{
    sraRectangleIterator* i=NULL;
    sraRect rect;
    int nUpdateRegionRects;
    rfbFramebufferUpdateMsg *fu = (rfbFramebufferUpdateMsg *)cl->updateBuf;
    sraRegionPtr updateRegion, updateCopyRegion, tmpRegion, cursorRegion;
    sraRect cursorBounds = { 0, 0, 0, 0 };
    int dx, dy;
    rfbBool sendCursorShape = FALSE;
    rfbBool sendCursorPos = FALSE;
    rfbBool cursorIsDrawn = FALSE;

    /*
     * If framebuffer size was changed and the client supports NewFBSize
     * encoding, just send NewFBSize marker and return.
     */

    if (cl->useNewFBSize && cl->newFBSizePending) {
      LOCK(cl->updateMutex);
      cl->newFBSizePending = FALSE;
      UNLOCK(cl->updateMutex);
      cl->rfbFramebufferUpdateMessagesSent++;
      fu->type = rfbFramebufferUpdate;
      fu->nRects = Swap16IfLE(1);
      cl->ublen = sz_rfbFramebufferUpdateMsg;
      if (!rfbSendNewFBSize(cl, cl->screen->width, cl->screen->height)) {
        return FALSE;
      }
      return rfbSendUpdateBuf(cl);
    }

    LOCK(cl->updateMutex);

    /*
     * The modifiedRegion may overlap the destination copyRegion.  We remove
     * any overlapping bits from the copyRegion (since they'd only be
     * overwritten anyway).
     */
    
    sraRgnSubtract(cl->copyRegion,cl->modifiedRegion);

    updateRegion = sraRgnCreateRgn(givenUpdateRegion);
    sraRgnOr(updateRegion,cl->copyRegion);

    /*
     * If the client doesn't support cursor updates, we want to draw
     * the cursor locally, send the update and then undraw it.
     * We only want to do this if the cursor has moved/changed or
     * if it is contained in updateRegion
     *
     * We also want to send an update for the region where the cursor
     * was last drawn, again only if the cursor has moved/changed.
     */

    LOCK(cl->screen->cursorMutex);

    cursorRegion = NULL;
    if (cl->enableCursorUpdates) {
	sendCursorShape = cl->cursorWasChanged;
	sendCursorPos   = cl->cursorWasMoved;
    } else {
	sraRegionPtr lastDrawnCursorRegion;

	if (rfbGetCursorBounds(cl->screen, &cursorBounds)) {
	    cursorRegion = sraRgnCreateRect(cursorBounds.x1, cursorBounds.y1,
					    cursorBounds.x2, cursorBounds.y2);
	}
	
	lastDrawnCursorRegion = sraRgnCreateRect(cl->lastDrawnCursorBounds.x1,
						 cl->lastDrawnCursorBounds.y1,
						 cl->lastDrawnCursorBounds.x2,
						 cl->lastDrawnCursorBounds.y2);

	if (cursorRegion) {
	    sraRgnOr(cursorRegion, lastDrawnCursorRegion);
	    sraRgnDestroy(lastDrawnCursorRegion);
	} else {
	    cursorRegion = lastDrawnCursorRegion;
	}

	if (cl->cursorWasChanged || cl->cursorWasMoved)
	    sraRgnOr(updateRegion, cursorRegion);
    }

    /*
     * The client is interested in the region requestedRegion.  The region
     * which should be updated now is the intersection of requestedRegion
     * and the union of modifiedRegion and copyRegion.  If it's empty then
     * no update is needed.
     */

    if (!sraRgnAnd(updateRegion, cl->requestedRegion) &&
	!sendCursorShape && !sendCursorPos) {
	sraRgnDestroy(updateRegion);
	if (cursorRegion)
	    sraRgnDestroy(cursorRegion);
	UNLOCK(cl->updateMutex);
	UNLOCK(cl->cursorMutex);
	return TRUE;
    }

    /*
     * Put up the cursor if any part of it is in updateRegion.
     */

    if (cursorRegion) {
	tmpRegion = sraRgnCreateRgn(cursorRegion);

	if (sraRgnAnd(tmpRegion, updateRegion)) {
	    rfbDrawCursor(cl->screen, &cursorBounds);
	    cursorIsDrawn = TRUE;

	    cl->cursorWasMoved = FALSE;
	    cl->cursorWasChanged = FALSE;
	    cl->lastDrawnCursorBounds = cursorBounds;

	    sraRgnOr(cl->modifiedRegion, cursorRegion);
	    
	    sraRgnOr(updateRegion, cursorRegion);
	    sraRgnAnd(updateRegion, cl->requestedRegion);
	}

	sraRgnDestroy(tmpRegion);
	sraRgnDestroy(cursorRegion);
    }

    /*
     * We assume that the client doesn't have any pixel data outside the
     * requestedRegion.  In other words, both the source and destination of a
     * copy must lie within requestedRegion.  So the region we can send as a
     * copy is the intersection of the copyRegion with both the requestedRegion
     * and the requestedRegion translated by the amount of the copy.  We set
     * updateCopyRegion to this.
     */

    updateCopyRegion = sraRgnCreateRgn(cl->copyRegion);
    sraRgnAnd(updateCopyRegion,cl->requestedRegion);
    tmpRegion = sraRgnCreateRgn(cl->requestedRegion);
    sraRgnOffset(tmpRegion,cl->copyDX,cl->copyDY);
    sraRgnAnd(updateCopyRegion,tmpRegion);
    sraRgnDestroy(tmpRegion);
    dx = cl->copyDX;
    dy = cl->copyDY;

    /*
     * Next we remove updateCopyRegion from updateRegion so that updateRegion
     * is the part of this update which is sent as ordinary pixel data (i.e not
     * a copy).
     */

    sraRgnSubtract(updateRegion,updateCopyRegion);

    /*
     * Finally we leave modifiedRegion to be the remainder (if any) of parts of
     * the screen which are modified but outside the requestedRegion.  We also
     * empty both the requestedRegion and the copyRegion - note that we never
     * carry over a copyRegion for a future update.
     */

     sraRgnOr(cl->modifiedRegion,cl->copyRegion);
     sraRgnSubtract(cl->modifiedRegion,updateRegion);
     sraRgnSubtract(cl->modifiedRegion,updateCopyRegion);

     sraRgnMakeEmpty(cl->requestedRegion);
     sraRgnMakeEmpty(cl->copyRegion);
     cl->copyDX = 0;
     cl->copyDY = 0;
   
     UNLOCK(cl->updateMutex);
   
   /*
     * Now send the update.
     */

    cl->rfbFramebufferUpdateMessagesSent++;

    if (cl->preferredEncoding == rfbEncodingCoRRE) {
        nUpdateRegionRects = 0;

        for(i = sraRgnGetIterator(updateRegion); sraRgnIteratorNext(i,&rect);){
            int x = rect.x1;
            int y = rect.y1;
            int w = rect.x2 - x;
            int h = rect.y2 - y;
            nUpdateRegionRects += (((w-1) / cl->correMaxWidth + 1)
                                     * ((h-1) / cl->correMaxHeight + 1));
        }
	sraRgnReleaseIterator(i);
#ifdef VINO_HAVE_ZLIB
    } else if (cl->preferredEncoding == rfbEncodingZlib) {
	nUpdateRegionRects = 0;

        for(i = sraRgnGetIterator(updateRegion); sraRgnIteratorNext(i,&rect);){
            int x = rect.x1;
            int y = rect.y1;
            int w = rect.x2 - x;
            int h = rect.y2 - y;
	    nUpdateRegionRects += (((h-1) / (ZLIB_MAX_SIZE( w ) / w)) + 1);
	}
	sraRgnReleaseIterator(i);
#ifdef VINO_HAVE_JPEG
    } else if (cl->preferredEncoding == rfbEncodingTight) {
	nUpdateRegionRects = 0;

        for(i = sraRgnGetIterator(updateRegion); sraRgnIteratorNext(i,&rect);){
            int x = rect.x1;
            int y = rect.y1;
            int w = rect.x2 - x;
            int h = rect.y2 - y;
	    int n = rfbNumCodedRectsTight(cl, x, y, w, h);
	    if (n == 0) {
		nUpdateRegionRects = 0xFFFF;
		break;
	    }
	    nUpdateRegionRects += n;
	}
	sraRgnReleaseIterator(i);
#endif
#endif
    } else {
        nUpdateRegionRects = sraRgnCountRects(updateRegion);
    }

    fu->type = rfbFramebufferUpdate;
    if (nUpdateRegionRects != 0xFFFF) {
	if(cl->screen->maxRectsPerUpdate>0
	   /* CoRRE splits the screen into smaller squares */
	   && cl->preferredEncoding != rfbEncodingCoRRE
#ifdef VINO_HAVE_ZLIB
	   /* Zlib encoding splits rectangles up into smaller chunks */
	   && cl->preferredEncoding != rfbEncodingZlib
#ifdef VINO_HAVE_JPEG
	   /* Tight encoding counts the rectangles differently */
	   && cl->preferredEncoding != rfbEncodingTight
#endif
#endif /* VINO_HAVE_ZLIB */
	   && nUpdateRegionRects>cl->screen->maxRectsPerUpdate) {
	    sraRegion* newUpdateRegion = sraRgnBBox(updateRegion);
	    sraRgnDestroy(updateRegion);
	    updateRegion = newUpdateRegion;
	    nUpdateRegionRects = sraRgnCountRects(updateRegion);
	}
	fu->nRects = Swap16IfLE((uint16_t)(sraRgnCountRects(updateCopyRegion) +
					   nUpdateRegionRects +
					   !!sendCursorShape + !!sendCursorPos));
    } else {
	fu->nRects = 0xFFFF;
    }
    cl->ublen = sz_rfbFramebufferUpdateMsg;

    UNLOCK(cl->cursorMutex);

   i = NULL;

   if (sendCursorShape) {
	cl->cursorWasChanged = FALSE;
	if (!rfbSendCursorShape(cl))
	    goto tx_error;
    }
   
   if (sendCursorPos) {
	cl->cursorWasMoved = FALSE;
	if (!rfbSendCursorPos(cl))
	    goto tx_error;
    }
   
    if (!sraRgnEmpty(updateCopyRegion)) {
	if (!rfbSendCopyRegion(cl,updateCopyRegion,dx,dy))
	    goto tx_error;
    }

    for(i = sraRgnGetIterator(updateRegion); sraRgnIteratorNext(i,&rect);){
        int x = rect.x1;
        int y = rect.y1;
        int w = rect.x2 - x;
        int h = rect.y2 - y;

        cl->rfbRawBytesEquivalent += (sz_rfbFramebufferUpdateRectHeader
                                      + w * (cl->format.bitsPerPixel / 8) * h);

        /* Validate the rectangle given by the update packet. */
        if (w + x > cl->screen->width || h + y > cl->screen->height)
            goto tx_error;

        switch (cl->preferredEncoding) {
        case rfbEncodingRaw:
            if (!rfbSendRectEncodingRaw(cl, x, y, w, h))
		goto tx_error;
            break;
        case rfbEncodingRRE:
            if (!rfbSendRectEncodingRRE(cl, x, y, w, h))
		goto tx_error;
            break;
        case rfbEncodingCoRRE:
            if (!rfbSendRectEncodingCoRRE(cl, x, y, w, h))
		goto tx_error;
            break;
        case rfbEncodingHextile:
            if (!rfbSendRectEncodingHextile(cl, x, y, w, h))
		goto tx_error;
            break;
#ifdef VINO_HAVE_ZLIB
	case rfbEncodingZlib:
	    if (!rfbSendRectEncodingZlib(cl, x, y, w, h))
		goto tx_error;
	    break;
#ifdef VINO_HAVE_JPEG
	case rfbEncodingTight:
	    if (!rfbSendRectEncodingTight(cl, x, y, w, h))
		goto tx_error;
	    break;
#endif
#endif
#ifdef VINO_HAVE_ZLIB
       case rfbEncodingZRLE:
           if (!rfbSendRectEncodingZRLE(cl, x, y, w, h))
	       goto tx_error;
           break;
#endif
        }
    }

    if (cursorIsDrawn)
	rfbUndrawCursor(cl->screen, &cursorBounds);

    if ( nUpdateRegionRects == 0xFFFF &&
	 !rfbSendLastRectMarker(cl) )
	goto tx_error;

    if (!rfbSendUpdateBuf(cl))
	goto tx_error;

    sraRgnReleaseIterator(i);
    sraRgnDestroy(updateCopyRegion);
    sraRgnDestroy(updateRegion);
    
    return TRUE;

 tx_error:
    if (cursorIsDrawn)
	rfbUndrawCursor(cl->screen, &cursorBounds);
    if (i)
	sraRgnReleaseIterator(i);

    sraRgnDestroy(updateCopyRegion);
    sraRgnDestroy(updateRegion);

    return FALSE;
}


/*
 * Send the copy region as a string of CopyRect encoded rectangles.
 * The only slightly tricky thing is that we should send the messages in
 * the correct order so that an earlier CopyRect will not corrupt the source
 * of a later one.
 */

rfbBool
rfbSendCopyRegion(rfbClientPtr cl,
                  sraRegionPtr reg,
                  int dx,
                  int dy)
{
    int x, y, w, h;
    rfbFramebufferUpdateRectHeader rect;
    rfbCopyRect cr;
    sraRectangleIterator* i;
    sraRect rect1;

    /* printf("copyrect: "); sraRgnPrint(reg); putchar('\n');fflush(stdout); */
    i = sraRgnGetReverseIterator(reg,dx>0,dy>0);

    while(sraRgnIteratorNext(i,&rect1)) {
      x = rect1.x1;
      y = rect1.y1;
      w = rect1.x2 - x;
      h = rect1.y2 - y;

      rect.r.x = Swap16IfLE(x);
      rect.r.y = Swap16IfLE(y);
      rect.r.w = Swap16IfLE(w);
      rect.r.h = Swap16IfLE(h);
      rect.encoding = Swap32IfLE(rfbEncodingCopyRect);

      memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,
	     sz_rfbFramebufferUpdateRectHeader);
      cl->ublen += sz_rfbFramebufferUpdateRectHeader;

      cr.srcX = Swap16IfLE(x - dx);
      cr.srcY = Swap16IfLE(y - dy);

      memcpy(&cl->updateBuf[cl->ublen], (char *)&cr, sz_rfbCopyRect);
      cl->ublen += sz_rfbCopyRect;

      cl->rfbRectanglesSent[rfbEncodingCopyRect]++;
      cl->rfbBytesSent[rfbEncodingCopyRect]
	+= sz_rfbFramebufferUpdateRectHeader + sz_rfbCopyRect;

    }

    sraRgnReleaseIterator(i);

    return TRUE;
}

/*
 * Send a given rectangle in raw encoding (rfbEncodingRaw).
 */

rfbBool
rfbSendRectEncodingRaw(rfbClientPtr cl,
                       int x,
                       int y,
                       int w,
                       int h)
{
    rfbFramebufferUpdateRectHeader rect;
    int nlines;
    int bytesPerLine = w * (cl->format.bitsPerPixel / 8);
    char *fbptr = (cl->screen->frameBuffer + (cl->screen->paddedWidthInBytes * y)
                   + (x * (cl->screen->bitsPerPixel / 8)));

    /* Flush the buffer to guarantee correct alignment for translateFn(). */
    if (cl->ublen > 0) {
        if (!rfbSendUpdateBuf(cl))
            return FALSE;
    }

    rect.r.x = Swap16IfLE(x);
    rect.r.y = Swap16IfLE(y);
    rect.r.w = Swap16IfLE(w);
    rect.r.h = Swap16IfLE(h);
    rect.encoding = Swap32IfLE(rfbEncodingRaw);

    memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,sz_rfbFramebufferUpdateRectHeader);
    cl->ublen += sz_rfbFramebufferUpdateRectHeader;

    cl->rfbRectanglesSent[rfbEncodingRaw]++;
    cl->rfbBytesSent[rfbEncodingRaw]
        += sz_rfbFramebufferUpdateRectHeader + bytesPerLine * h;

    nlines = (UPDATE_BUF_SIZE - cl->ublen) / bytesPerLine;

    while (TRUE) {
        if (nlines > h)
            nlines = h;

        (*cl->translateFn)(cl->translateLookupTable,
			   &(cl->screen->rfbServerFormat),
                           &cl->format, fbptr, &cl->updateBuf[cl->ublen],
                           cl->screen->paddedWidthInBytes, w, nlines);

        cl->ublen += nlines * bytesPerLine;
        h -= nlines;

        if (h == 0)     /* rect fitted in buffer, do next one */
            return TRUE;

        /* buffer full - flush partial rect and do another nlines */

        if (!rfbSendUpdateBuf(cl))
            return FALSE;

        fbptr += (cl->screen->paddedWidthInBytes * nlines);

        nlines = (UPDATE_BUF_SIZE - cl->ublen) / bytesPerLine;
        if (nlines == 0) {
            rfbErr("rfbSendRectEncodingRaw: send buffer too small for %d "
                   "bytes per line\n", bytesPerLine);
            rfbCloseClient(cl);
            return FALSE;
        }
    }
}



/*
 * Send an empty rectangle with encoding field set to value of
 * rfbEncodingLastRect to notify client that this is the last
 * rectangle in framebuffer update ("LastRect" extension of RFB
 * protocol).
 */

rfbBool
rfbSendLastRectMarker(rfbClientPtr cl)
{
    rfbFramebufferUpdateRectHeader rect;

    if (cl->ublen + sz_rfbFramebufferUpdateRectHeader > UPDATE_BUF_SIZE) {
	if (!rfbSendUpdateBuf(cl))
	    return FALSE;
    }

    rect.encoding = Swap32IfLE(rfbEncodingLastRect);
    rect.r.x = 0;
    rect.r.y = 0;
    rect.r.w = 0;
    rect.r.h = 0;

    memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,sz_rfbFramebufferUpdateRectHeader);
    cl->ublen += sz_rfbFramebufferUpdateRectHeader;

    cl->rfbLastRectMarkersSent++;
    cl->rfbLastRectBytesSent += sz_rfbFramebufferUpdateRectHeader;

    return TRUE;
}


/*
 * Send NewFBSize pseudo-rectangle. This tells the client to change
 * its framebuffer size.
 */

rfbBool
rfbSendNewFBSize(rfbClientPtr cl,
                 int w,
                 int h)
{
    rfbFramebufferUpdateRectHeader rect;

    if (cl->ublen + sz_rfbFramebufferUpdateRectHeader > UPDATE_BUF_SIZE) {
	if (!rfbSendUpdateBuf(cl))
	    return FALSE;
    }

    rect.encoding = Swap32IfLE(rfbEncodingNewFBSize);
    rect.r.x = 0;
    rect.r.y = 0;
    rect.r.w = Swap16IfLE(w);
    rect.r.h = Swap16IfLE(h);

    memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,
           sz_rfbFramebufferUpdateRectHeader);
    cl->ublen += sz_rfbFramebufferUpdateRectHeader;

    cl->rfbLastRectMarkersSent++;
    cl->rfbLastRectBytesSent += sz_rfbFramebufferUpdateRectHeader;

    return TRUE;
}


/*
 * Send the contents of cl->updateBuf.  Returns 1 if successful, -1 if
 * not (errno should be set).
 */

rfbBool
rfbSendUpdateBuf(rfbClientPtr cl)
{
    if(cl->sock<0)
      return FALSE;

    if (WriteExact(cl, cl->updateBuf, cl->ublen) < 0) {
        rfbLogPerror("rfbSendUpdateBuf: write");
        rfbCloseClient(cl);
        return FALSE;
    }

    cl->ublen = 0;
    return TRUE;
}

/*
 * rfbSendSetColourMapEntries sends a SetColourMapEntries message to the
 * client, using values from the currently installed colormap.
 */

rfbBool
rfbSendSetColourMapEntries(rfbClientPtr cl,
                           int firstColour,
                           int nColours)
{
    char buf[sz_rfbSetColourMapEntriesMsg + 256 * 3 * 2];
    rfbSetColourMapEntriesMsg *scme = (rfbSetColourMapEntriesMsg *)buf;
    uint16_t *rgb = (uint16_t *)(&buf[sz_rfbSetColourMapEntriesMsg]);
    rfbColourMap* cm = &cl->screen->colourMap;
    
    int i, len;

    scme->type = rfbSetColourMapEntries;

    scme->firstColour = Swap16IfLE(firstColour);
    scme->nColours = Swap16IfLE(nColours);

    len = sz_rfbSetColourMapEntriesMsg;

    for (i = 0; i < nColours; i++) {
      if(i<(int)cm->count) {
	if(cm->is16) {
	  rgb[i*3] = Swap16IfLE(cm->data.shorts[i*3]);
	  rgb[i*3+1] = Swap16IfLE(cm->data.shorts[i*3+1]);
	  rgb[i*3+2] = Swap16IfLE(cm->data.shorts[i*3+2]);
	} else {
	  rgb[i*3] = Swap16IfLE(cm->data.bytes[i*3]);
	  rgb[i*3+1] = Swap16IfLE(cm->data.bytes[i*3+1]);
	  rgb[i*3+2] = Swap16IfLE(cm->data.bytes[i*3+2]);
	}
      }
    }

    len += nColours * 3 * 2;

    if (WriteExact(cl, buf, len) < 0) {
	rfbLogPerror("rfbSendSetColourMapEntries: write");
	rfbCloseClient(cl);
	return FALSE;
    }
    return TRUE;
}

/*
 * rfbSendBell sends a Bell message to all the clients.
 */

void
rfbSendBell(rfbScreenInfoPtr rfbScreen)
{
    rfbClientIteratorPtr i;
    rfbClientPtr cl;
    rfbBellMsg b;

    i = rfbGetClientIterator(rfbScreen);
    while((cl=rfbClientIteratorNext(i))) {
	b.type = rfbBell;
	if (WriteExact(cl, (char *)&b, sz_rfbBellMsg) < 0) {
	    rfbLogPerror("rfbSendBell: write");
	    rfbCloseClient(cl);
	}
    }
    rfbReleaseClientIterator(i);
}


/*
 * rfbSendServerCutText sends a ServerCutText message to all the authenticated
 * clients.
 */

void
rfbSendServerCutText(rfbScreenInfoPtr rfbScreen,char *str, int len)
{
    rfbClientPtr cl;
    rfbServerCutTextMsg sct;
    rfbClientIteratorPtr iterator;

    iterator = rfbGetClientIterator(rfbScreen);
    while ((cl = rfbClientIteratorNext(iterator)) != NULL) {
        /* Client is not authenticated, ignore. See GNOME bug 678434. */
        if (cl->state != RFB_NORMAL)
            continue;

        sct.type = rfbServerCutText;
        sct.length = Swap32IfLE(len);
        if (WriteExact(cl, (char *)&sct,
                       sz_rfbServerCutTextMsg) < 0) {
            rfbLogPerror("rfbSendServerCutText: write");
            rfbCloseClient(cl);
            continue;
        }
        if (WriteExact(cl, str, len) < 0) {
            rfbLogPerror("rfbSendServerCutText: write");
            rfbCloseClient(cl);
        }
    }
    rfbReleaseClientIterator(iterator);
}
