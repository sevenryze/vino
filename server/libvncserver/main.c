/*
 *  This file is called main.c, because it contains most of the new functions
 *  for use with LibVNCServer.
 *
 *  LibVNCServer (C) 2001 Johannes E. Schindelin <Johannes.Schindelin@gmx.de>
 *  Original OSXvnc (C) 2001 Dan McGuirk <mcguirk@incompleteness.net>.
 *  Original Xvnc (C) 1999 AT&T Laboratories Cambridge.  
 *  All Rights Reserved.
 *
 *  see GPL (latest version) for full details
 */

#include "rfb/rfb.h"
#include "rfb/rfbregion.h"

#include <stdarg.h>
#include <errno.h>

#ifndef false
#define false 0
#define true -1
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include <signal.h>
#include <time.h>

#include <glib.h>

int rfbEnableLogging=1;

#ifdef WORDS_BIGENDIAN
char rfbEndianTest = 0;
#else
char rfbEndianTest = -1;
#endif

void rfbLogEnable(int enabled) {
  rfbEnableLogging=enabled;
}

/*
 * rfbLog prints a time-stamped message to the log file (stderr).
 */

static void rfbDefaultLog(const char *format, ...) G_GNUC_PRINTF (1, 2);

static void
rfbDefaultLog(const char *format, ...)
{
    va_list args;
    char buf[256];
    time_t log_clock;

    if(!rfbEnableLogging)
      return;

    LOCK(logMutex);
    va_start(args, format);

    time(&log_clock);
    strftime(buf, 255, "%d/%m/%Y %X ", localtime(&log_clock));
    fprintf(stderr, "%s", buf);

    vfprintf(stderr, format, args);
    fflush(stderr);

    va_end(args);
    UNLOCK(logMutex);
}

rfbLogProc rfbLog=rfbDefaultLog;
rfbLogProc rfbErr=rfbDefaultLog;

void rfbLogPerror(const char *str)
{
    rfbErr("%s: %s\n", str, strerror(errno));
}

void rfbScheduleCopyRegion(rfbScreenInfoPtr rfbScreen,sraRegionPtr copyRegion,int dx,int dy)
{  
   rfbClientIteratorPtr iterator;
   rfbClientPtr cl;

   iterator=rfbGetClientIterator(rfbScreen);
   while((cl=rfbClientIteratorNext(iterator))) {
     LOCK(cl->updateMutex);
     if(cl->useCopyRect) {
       sraRegionPtr modifiedRegionBackup;
       if(!sraRgnEmpty(cl->copyRegion)) {
	  if(cl->copyDX!=dx || cl->copyDY!=dy) {
	     /* if a copyRegion was not yet executed, treat it as a
	      * modifiedRegion. The idea: in this case it could be
	      * source of the new copyRect or modified anyway. */
	     sraRgnOr(cl->modifiedRegion,cl->copyRegion);
	     sraRgnMakeEmpty(cl->copyRegion);
	  } else {
	     /* we have to set the intersection of the source of the copy
	      * and the old copy to modified. */
	     modifiedRegionBackup=sraRgnCreateRgn(copyRegion);
	     sraRgnOffset(modifiedRegionBackup,-dx,-dy);
	     sraRgnAnd(modifiedRegionBackup,cl->copyRegion);
	     sraRgnOr(cl->modifiedRegion,modifiedRegionBackup);
	     sraRgnDestroy(modifiedRegionBackup);
	  }
       }
	  
       sraRgnOr(cl->copyRegion,copyRegion);
       cl->copyDX = dx;
       cl->copyDY = dy;

       /* if there were modified regions, which are now copied,
	* mark them as modified, because the source of these can be overlapped
	* either by new modified or now copied regions. */
       modifiedRegionBackup=sraRgnCreateRgn(cl->modifiedRegion);
       sraRgnOffset(modifiedRegionBackup,dx,dy);
       sraRgnAnd(modifiedRegionBackup,cl->copyRegion);
       sraRgnOr(cl->modifiedRegion,modifiedRegionBackup);
       sraRgnDestroy(modifiedRegionBackup);

#if 0
       /* TODO: is this needed? Or does it mess up deferring? */
       /* while(!sraRgnEmpty(cl->copyRegion)) */ {
	   {
	     sraRegionPtr updateRegion = sraRgnCreateRgn(cl->modifiedRegion);
	     sraRgnOr(updateRegion,cl->copyRegion);
	     UNLOCK(cl->updateMutex);
	     rfbSendFramebufferUpdate(cl,updateRegion);
	     sraRgnDestroy(updateRegion);
	     continue;
	   }
       }
#endif
     } else {
       sraRgnOr(cl->modifiedRegion,copyRegion);
     }
     UNLOCK(cl->updateMutex);
   }

   rfbReleaseClientIterator(iterator);
}

void rfbDoCopyRegion(rfbScreenInfoPtr rfbScreen,sraRegionPtr copyRegion,int dx,int dy)
{
   sraRectangleIterator* i;
   sraRect rect;
   int j,widthInBytes,bpp=rfbScreen->rfbServerFormat.bitsPerPixel/8,
    rowstride=rfbScreen->paddedWidthInBytes;
   char *in,*out;

   /* copy it, really */
   i = sraRgnGetReverseIterator(copyRegion,dx<0,dy<0);
   while(sraRgnIteratorNext(i,&rect)) {
     widthInBytes = (rect.x2-rect.x1)*bpp;
     out = rfbScreen->frameBuffer+rect.x1*bpp+rect.y1*rowstride;
     in = rfbScreen->frameBuffer+(rect.x1-dx)*bpp+(rect.y1-dy)*rowstride;
     if(dy<0)
       for(j=rect.y1;j<rect.y2;j++,out+=rowstride,in+=rowstride)
	 memmove(out,in,widthInBytes);
     else {
       out += rowstride*(rect.y2-rect.y1-1);
       in += rowstride*(rect.y2-rect.y1-1);
       for(j=rect.y2-1;j>=rect.y1;j--,out-=rowstride,in-=rowstride)
	 memmove(out,in,widthInBytes);
     }
   }
  
   rfbScheduleCopyRegion(rfbScreen,copyRegion,dx,dy);

   sraRgnReleaseIterator(i);
}

void rfbDoCopyRect(rfbScreenInfoPtr rfbScreen,int x1,int y1,int x2,int y2,int dx,int dy)
{
  sraRegionPtr region = sraRgnCreateRect(x1,y1,x2,y2);
  rfbDoCopyRegion(rfbScreen,region,dx,dy);
}

void rfbScheduleCopyRect(rfbScreenInfoPtr rfbScreen,int x1,int y1,int x2,int y2,int dx,int dy)
{
  sraRegionPtr region = sraRgnCreateRect(x1,y1,x2,y2);
  rfbScheduleCopyRegion(rfbScreen,region,dx,dy);
}

void rfbMarkRegionAsModified(rfbScreenInfoPtr rfbScreen,sraRegionPtr modRegion)
{
   rfbClientIteratorPtr iterator;
   rfbClientPtr cl;

   iterator=rfbGetClientIterator(rfbScreen);
   while((cl=rfbClientIteratorNext(iterator))) {
     LOCK(cl->updateMutex);
     sraRgnOr(cl->modifiedRegion,modRegion);
     UNLOCK(cl->updateMutex);
   }

   rfbReleaseClientIterator(iterator);
}

void rfbMarkRectAsModified(rfbScreenInfoPtr rfbScreen,int x1,int y1,int x2,int y2)
{
   sraRegionPtr region;
   int i;

   if(x1>x2) { i=x1; x1=x2; x2=i; }
   if(x1<0) x1=0;
   if(x2>=rfbScreen->width) x2=rfbScreen->width-1;
   if(x1==x2) return;
   
   if(y1>y2) { i=y1; y1=y2; y2=i; }
   if(y1<0) y1=0;
   if(y2>=rfbScreen->height) y2=rfbScreen->height-1;
   if(y1==y2) return;
   
   region = sraRgnCreateRect(x1,y1,x2,y2);
   rfbMarkRegionAsModified(rfbScreen,region);
   sraRgnDestroy(region);
}

void
defaultPtrAddEvent(int buttonMask, int x, int y, rfbClientPtr cl)
{
  rfbSetCursorPosition(cl->screen, cl, x, y);
}

/* TODO: add a nice VNC or RFB cursor */

#if defined(WIN32) || defined(sparc) || !defined(NO_STRICT_ANSI)
static rfbCursor myCursor = 
{
   FALSE, FALSE, FALSE, FALSE,
   (unsigned char*)"\000\102\044\030\044\102\000",
   (unsigned char*)"\347\347\176\074\176\347\347",
   8, 7, 3, 3,
   0, 0, 0,
   0xffff, 0xffff, 0xffff,
   0
};
#else
static rfbCursor myCursor = 
{
   cleanup: FALSE,
   cleanupSource: FALSE,
   cleanupMask: FALSE,
   cleanupRichSource: FALSE,
   source: "\000\102\044\030\044\102\000",
   mask:   "\347\347\176\074\176\347\347",
   width: 8, height: 7, xhot: 3, yhot: 3,
   foreRed: 0, foreGreen: 0, foreBlue: 0,
   backRed: 0xffff, backGreen: 0xffff, backBlue: 0xffff,
   richSource: 0
};
#endif

/*
 * Update server's pixel format in rfbScreenInfo structure. This
 * function is called from rfbGetScreen() and rfbNewFramebuffer().
 */

static void rfbInitServerFormat(rfbScreenInfoPtr rfbScreen, int bitsPerSample)
{
   rfbPixelFormat* format=&rfbScreen->rfbServerFormat;

   format->bitsPerPixel = rfbScreen->bitsPerPixel;
   format->depth = rfbScreen->depth;
   format->bigEndian = rfbEndianTest?FALSE:TRUE;
   format->trueColour = TRUE;
   rfbScreen->colourMap.count = 0;
   rfbScreen->colourMap.is16 = 0;
   rfbScreen->colourMap.data.bytes = NULL;

   if (format->bitsPerPixel == 8) {
     format->redMax = 7;
     format->greenMax = 7;
     format->blueMax = 3;
     format->redShift = 0;
     format->greenShift = 3;
     format->blueShift = 6;
   } else {
     format->redMax = (1 << bitsPerSample) - 1;
     format->greenMax = (1 << bitsPerSample) - 1;
     format->blueMax = (1 << bitsPerSample) - 1;
     if(rfbEndianTest) {
       format->redShift = 0;
       format->greenShift = bitsPerSample;
       format->blueShift = bitsPerSample * 2;
     } else {
       if(format->bitsPerPixel==8*3) {
	 format->redShift = bitsPerSample*2;
	 format->greenShift = bitsPerSample*1;
	 format->blueShift = 0;
       } else {
	 format->redShift = bitsPerSample*3;
	 format->greenShift = bitsPerSample*2;
	 format->blueShift = bitsPerSample;
       }
     }
   }
}

rfbScreenInfoPtr rfbGetScreen(int* argc,char** argv,
 int width,int height,int bitsPerSample,int samplesPerPixel,
 int bytesPerPixel)
{
   rfbScreenInfoPtr rfbScreen=malloc(sizeof(rfbScreenInfo));

   if(width&3)
     rfbErr("WARNING: Width (%d) is not a multiple of 4. VncViewer has problems with that.\n",width);

   rfbScreen->autoPort=FALSE;
   rfbScreen->localOnly=FALSE;
   rfbScreen->rfbClientHead=0;
   rfbScreen->rfbPort=5900;
   rfbScreen->socketInitDone=FALSE;

   rfbScreen->inetdInitDone = FALSE;
   rfbScreen->inetdSock=-1;

   rfbScreen->maxFd=0;

   rfbScreen->rfbListenSock[0] = -1;
   rfbScreen->rfbListenSockTotal = 0;
   rfbScreen->netIface = NULL;

   rfbScreen->desktopName = strdup("LibVNCServer");
   rfbScreen->rfbAlwaysShared = FALSE;
   rfbScreen->rfbNeverShared = FALSE;
   rfbScreen->rfbDontDisconnect = FALSE;
   
   rfbScreen->width = width;
   rfbScreen->height = height;
   rfbScreen->bitsPerPixel = rfbScreen->depth = 8*bytesPerPixel;

   rfbScreen->securityTypes[0] = rfbNoAuth;
   rfbScreen->nSecurityTypes = 0;
   rfbScreen->authTypes[0] = rfbNoAuth;
   rfbScreen->nAuthTypes = 0;
   rfbScreen->passwordCheck = NULL;

#ifdef WIN32
   {
	   DWORD dummy=255;
	   GetComputerName(rfbScreen->rfbThisHost,&dummy);
   }
#else
   gethostname(rfbScreen->rfbThisHost, 255);
#endif

   rfbScreen->paddedWidthInBytes = width*bytesPerPixel;

   /* format */

   rfbInitServerFormat(rfbScreen, bitsPerSample);

   /* cursor */

   rfbScreen->cursorX=rfbScreen->cursorY=rfbScreen->underCursorBufferLen=0;
   rfbScreen->underCursorBuffer=NULL;
   rfbScreen->cursor = &myCursor;

   rfbScreen->rfbDeferUpdateTime=5;
   rfbScreen->maxRectsPerUpdate=50;

   /* proc's and hook's */

   rfbScreen->kbdAddEvent = NULL;
   rfbScreen->ptrAddEvent = defaultPtrAddEvent;
   rfbScreen->setXCutText = NULL;
   rfbScreen->newClientHook = NULL;
   rfbScreen->authenticatedClientHook = NULL;

   /* initialize client list and iterator mutex */
   rfbClientListInit(rfbScreen);

   rfbAuthInitScreen(rfbScreen);

   return(rfbScreen);
}

/*
 * Switch to another sized framebuffer. Clients supporting NewFBSize
 * pseudo-encoding will change their local framebuffer dimensions
 * if necessary.
 * NOTE: Rich cursor data should be converted to new pixel format by
 * the caller.
 */

void rfbNewFramebuffer(rfbScreenInfoPtr rfbScreen, char *framebuffer,
                       int width, int height)
{
  rfbClientIteratorPtr iterator;
  rfbClientPtr cl;

  if (width & 3)
    rfbErr("WARNING: New width (%d) is not a multiple of 4.\n", width);

  /* Update information in the rfbScreenInfo structure */

  rfbScreen->width = width;
  rfbScreen->height = height;
  rfbScreen->frameBuffer = framebuffer;

  /* Adjust pointer position if necessary */

  if (rfbScreen->cursorX >= width)
    rfbScreen->cursorX = width - 1;
  if (rfbScreen->cursorY >= height)
    rfbScreen->cursorY = height - 1;

  /* For each client: */
  iterator = rfbGetClientIterator(rfbScreen);
  while ((cl = rfbClientIteratorNext(iterator)) != NULL) {

    /* Mark the screen contents as changed, and schedule sending
       NewFBSize message if supported by this client. */

    LOCK(cl->updateMutex);
    sraRgnDestroy(cl->modifiedRegion);
    cl->modifiedRegion = sraRgnCreateRect(0, 0, width, height);
    sraRgnMakeEmpty(cl->copyRegion);
    cl->copyDX = 0;
    cl->copyDY = 0;

    if (cl->useNewFBSize)
      cl->newFBSizePending = TRUE;

    UNLOCK(cl->updateMutex);
  }
  rfbReleaseClientIterator(iterator);
}

void rfbSetDesktopName(rfbScreenInfoPtr rfbScreen, const char *name)
{
  if (rfbScreen->desktopName)
    free(rfbScreen->desktopName);
  rfbScreen->desktopName = strdup(name);
}

void rfbScreenCleanup(rfbScreenInfoPtr rfbScreen)
{
  rfbClientIteratorPtr i=rfbGetClientIterator(rfbScreen);
  rfbClientPtr cl,cl1=rfbClientIteratorNext(i);
  while(cl1) {
    cl=rfbClientIteratorNext(i);
    rfbClientConnectionGone(cl1);
    cl1=cl;
  }
  rfbReleaseClientIterator(i);

  rfbAuthCleanupScreen(rfbScreen);
    
  /* TODO: hang up on all clients and free all reserved memory */
#define FREE_IF(x) if(rfbScreen->x) free(rfbScreen->x)
  FREE_IF(colourMap.data.bytes);
  FREE_IF(underCursorBuffer);
  if(rfbScreen->cursor)
    rfbFreeCursor(rfbScreen->cursor);
  if(rfbScreen->desktopName)
    free(rfbScreen->desktopName);
  free(rfbScreen);
#ifdef VINO_HAVE_JPEG
  rfbTightCleanup();
#endif
}

void rfbInitServer(rfbScreenInfoPtr rfbScreen)
{
#ifdef WIN32
  WSADATA trash;
  int i=WSAStartup(MAKEWORD(2,2),&trash);
#endif
  rfbInitSockets(rfbScreen);
}

#ifndef HAVE_GETTIMEOFDAY
#include <fcntl.h>
#include <conio.h>
#include <sys/timeb.h>

void gettimeofday(struct timeval* tv,char* dummy)
{
   SYSTEMTIME t;
   GetSystemTime(&t);
   tv->tv_sec=t.wHour*3600+t.wMinute*60+t.wSecond;
   tv->tv_usec=t.wMilliseconds*1000;
}
#endif

void
rfbUpdateClient(rfbClientPtr cl)
{
  struct timeval tv;
  
  if (cl->sock >= 0 && !cl->onHold && FB_UPDATE_PENDING(cl) &&
      !sraRgnEmpty(cl->requestedRegion)) {
    if(cl->screen->rfbDeferUpdateTime == 0) {
      rfbSendFramebufferUpdate(cl,cl->modifiedRegion);
    } else if(cl->startDeferring.tv_usec == 0) {
      gettimeofday(&cl->startDeferring,NULL);
      if(cl->startDeferring.tv_usec == 0)
	cl->startDeferring.tv_usec++;
    } else {
      gettimeofday(&tv,NULL);
      if(tv.tv_sec < cl->startDeferring.tv_sec /* at midnight */
	 || ((tv.tv_sec-cl->startDeferring.tv_sec)*1000
	     +(tv.tv_usec-cl->startDeferring.tv_usec)/1000)
	 > cl->screen->rfbDeferUpdateTime) {
	cl->startDeferring.tv_usec = 0;
	rfbSendFramebufferUpdate(cl,cl->modifiedRegion);
      }
    }
  }
}

void
rfbProcessEvents(rfbScreenInfoPtr rfbScreen,long usec)
{
  rfbClientIteratorPtr i;
  rfbClientPtr cl,clPrev;

  if(usec<0)
    usec=rfbScreen->rfbDeferUpdateTime*1000;

  rfbCheckFds(rfbScreen,usec);

  i = rfbGetClientIterator(rfbScreen);
  cl=rfbClientIteratorHead(i);
  while(cl) {
    rfbUpdateClient(cl);
    clPrev=cl;
    cl=rfbClientIteratorNext(i);
    if(clPrev->sock==-1)
      rfbClientConnectionGone(clPrev);
  }
  rfbReleaseClientIterator(i);
}

void rfbRunEventLoop(rfbScreenInfoPtr rfbScreen, long usec, rfbBool runInBackground)
{
  if(runInBackground) {
    rfbErr("Can't run in background, because I don't have PThreads!\n");
    return;
  }

  if(usec<0)
    usec=rfbScreen->rfbDeferUpdateTime*1000;

  while(1)
    rfbProcessEvents(rfbScreen,usec);
}

static char *
securityTypeToName(int securityType)
{
    switch (securityType) {
    case rfbNoAuth:
	return "No Authentication";
    case rfbVncAuth:
	return "VNC Authentication";
#ifdef VINO_HAVE_GNUTLS
    case rfbTLS:
	return "TLS";
#endif
    default:
	return "unknown";
    }
}

void rfbAddSecurityType(rfbScreenInfoPtr rfbScreen, int securityType)
{
    if (rfbScreen->nSecurityTypes >= RFB_MAX_N_SECURITY_TYPES)
	return;

    rfbLog("Advertising security type: '%s' (%d)\n",
	   securityTypeToName(securityType), securityType);
    
    switch (securityType) {
    case rfbNoAuth:
    case rfbVncAuth:
#ifdef VINO_HAVE_GNUTLS
    case rfbTLS:
#endif
	rfbScreen->securityTypes[rfbScreen->nSecurityTypes] = securityType;
	rfbScreen->nSecurityTypes++;
	break;
    default:
	break;
    }
}

void rfbClearSecurityTypes(rfbScreenInfoPtr rfbScreen)
{
    if (rfbScreen->nSecurityTypes > 0) {
	rfbLog("Clearing securityTypes\n");

	memset (&rfbScreen->securityTypes, 0, sizeof (rfbScreen->securityTypes));
	rfbScreen->securityTypes [0] = rfbNoAuth;
	rfbScreen->nSecurityTypes = 0;
    }
}

static char *
authTypeToName(int authType)
{
    switch (authType) {
    case rfbNoAuth:
	return "No Authentication";
    case rfbVncAuth:
	return "VNC Authentication";
    default:
	return "unknown";
    }
}

void rfbAddAuthType(rfbScreenInfoPtr rfbScreen, int authType)
{
    if (rfbScreen->nAuthTypes >= RFB_MAX_N_AUTH_TYPES)
	return;

    rfbLog("Advertising authentication type: '%s' (%d)\n",
	   authTypeToName(authType), authType);
    
    switch (authType) {
    case rfbNoAuth:
    case rfbVncAuth:
	rfbScreen->authTypes[rfbScreen->nAuthTypes] = authType;
	rfbScreen->nAuthTypes++;
	break;
    default:
	break;
    }
}

void rfbClearAuthTypes(rfbScreenInfoPtr rfbScreen)
{
    if (rfbScreen->nAuthTypes > 0) {
	rfbLog("Clearing authTypes\n");

	memset (&rfbScreen->authTypes, 0, sizeof (rfbScreen->authTypes));
	rfbScreen->authTypes [0] = rfbNoAuth;
	rfbScreen->nAuthTypes = 0;
    }
}
