/*
 * Copyright (C) 2003 Sun Microsystems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Authors:
 *      Mark McLoughlin <mark@skynet.ie>
 */

#include <config.h>

#include "vino-server.h"

#include <rfb/rfb.h>
#include "vino-fb.h"
#ifdef VINO_ENABLE_HTTP_SERVER
#include "vino-http.h"
#endif
#include "vino-input.h"
#include "vino-cursor.h"
#include "vino-prompt.h"
#include "vino-util.h"
#include "vino-enums.h"

/* If an authentication attempt failes, delay the next
 * authentication attempt for 5 seconds.
 */
#define VINO_SERVER_AUTH_DEFER_LEN 5

struct _VinoServerPrivate
{
  rfbScreenInfoPtr  rfb_screen;

  GdkScreen        *screen;
  VinoFB           *fb;
  VinoCursorData   *cursor_data;
  VinoPrompt       *prompt;

  GIOChannel       *io_channel;
  guint             io_watch;

  GSList           *clients;

  VinoAuthMethod    auth_methods;
  char             *vnc_password;

#ifdef VINO_ENABLE_HTTP_SERVER
  VinoHTTP         *http;
#endif

  guint             on_hold : 1;
  guint             prompt_enabled : 1;
  guint             view_only : 1;
  guint             require_encryption : 1;
  guint             last_auth_failed : 1;
};

typedef struct
{
  rfbClientPtr  rfb_client;
  GIOChannel   *io_channel;
  guint         io_watch;
  guint         update_timeout;

  /* Deferred authentication */
  guint         auth_timeout;
  char         *auth_response;
  int           auth_resp_len;
} VinoServerClientInfo;

enum
{
  PROP_0,
  PROP_SCREEN,
  PROP_ON_HOLD,
  PROP_PROMPT_ENABLED,
  PROP_VIEW_ONLY,
  PROP_REQUIRE_ENCRYPTION,
  PROP_AUTH_METHODS,
  PROP_VNC_PASSWORD
};

static enum rfbNewClientAction vino_server_auth_client (VinoServer           *server,
							VinoServerClientInfo *client,
							const char           *response,
							int                   length);

static void vino_server_setup_framebuffer   (VinoServer *server);
static void vino_server_release_framebuffer (VinoServer *server);

static gpointer parent_class;

static void
vino_server_handle_client_gone (rfbClientPtr rfb_client)
{
  VinoServer           *server = VINO_SERVER (rfb_client->screen->screenData);
  VinoServerClientInfo *client = NULL;
  GSList               *l;

  g_return_if_fail (VINO_IS_SERVER (server));

  dprintf (RFB, "Client with fd == %d gone\n", rfb_client->sock);

  vino_prompt_remove_client (server->priv->prompt, rfb_client);

  for (l = server->priv->clients; l; l = l->next)
    {
      client = (VinoServerClientInfo *) l->data;

      if (rfb_client == client->rfb_client)
	{
	  dprintf (RFB, "Found client %p ... cleaning up\n", client->rfb_client);

	  if (client->auth_timeout)
	    g_source_remove (client->auth_timeout);
	  client->auth_timeout = 0;

	  if (client->auth_response)
	    g_free (client->auth_response);
	  client->auth_response = NULL;
	  client->auth_resp_len = 0;

	  if (client->update_timeout)
	    g_source_remove (client->update_timeout);
	  client->update_timeout = 0;

	  g_source_remove (client->io_watch);
	  client->io_watch = 0;

	  g_io_channel_unref (client->io_channel);
	  client->io_channel = NULL;

	  g_free (l->data);
	  server->priv->clients = g_slist_delete_link (server->priv->clients, l);
	  break;
	}
    }

  if (!server->priv->clients)
    vino_server_release_framebuffer (server);
}

static gboolean
vino_server_update_client (rfbClientPtr rfb_client)
{
  VinoServer *server = VINO_SERVER (rfb_client->screen->screenData);
  const char *cursor_source;
  const char *cursor_mask;
  int         x, y;
  int         width, height;

  g_return_val_if_fail (VINO_IS_SERVER (server), FALSE);

  if (vino_cursor_get_x_source (server->priv->cursor_data,
				&width, &height,
				&cursor_source, &cursor_mask))
    {
      rfbCursorPtr rfb_cursor;

      rfb_cursor = rfbMakeXCursor (width, height, cursor_source, cursor_mask);
      rfbSetCursor (rfb_client->screen, rfb_cursor, TRUE);
    }

  vino_cursor_get_position (server->priv->cursor_data, &x, &y);
  rfbSetCursorPosition (rfb_client->screen, NULL, x, y);

  rfbUpdateClient (rfb_client);

  if (rfb_client->sock == -1)
    {
      rfbClientConnectionGone (rfb_client);
      return FALSE;
    }
  
  return TRUE;
}

static gboolean
vino_server_update_client_timeout (rfbClientPtr rfb_client)
{
  if (rfb_client->onHold)
    return TRUE;

  vino_server_update_client (rfb_client);

  return TRUE;
}

static void
vino_server_set_client_on_hold (VinoServer            *server,
				VinoServerClientInfo  *client,
				gboolean               on_hold)
{
  rfbClientPtr rfb_client = client->rfb_client;

  dprintf (RFB, "Setting client '%s' on hold: %s\n",
	   rfb_client->host, on_hold ? "(true)" : "(false)");

  rfb_client->onHold = on_hold;

  /* We don't process any pending data from an client which is
   * on hold, so don't let it starve the rest of the mainloop.
   */
  g_source_set_priority (g_main_context_find_source_by_id (NULL, client->io_watch),
			 on_hold ? G_PRIORITY_LOW : G_PRIORITY_DEFAULT);

  if (!on_hold)
    {
      if (!client->update_timeout)
	client->update_timeout = g_timeout_add (50,
						(GSourceFunc) vino_server_update_client_timeout,
						rfb_client);
    }
  else
    {
      if (client->update_timeout)
	g_source_remove (client->update_timeout);
      client->update_timeout = 0;
    }
}

static gboolean
vino_server_client_data_pending (GIOChannel   *source,
				 GIOCondition  condition,
				 rfbClientPtr  rfb_client)
{
  if (rfb_client->onHold)
    return TRUE;

  rfbProcessClientMessage (rfb_client);
  
  return vino_server_update_client (rfb_client);
}

static enum rfbNewClientAction
vino_server_handle_new_client (rfbClientPtr rfb_client)
{
  VinoServer           *server = VINO_SERVER (rfb_client->screen->screenData);
  VinoServerClientInfo *client;

  g_return_val_if_fail (VINO_IS_SERVER (server), RFB_CLIENT_REFUSE);

  dprintf (RFB, "New client on fd %d\n", rfb_client->sock);

  if (!server->priv->fb)
    vino_server_setup_framebuffer (server);

  client = g_new0 (VinoServerClientInfo, 1);

  rfb_client->clientData = client;

  client->rfb_client = rfb_client;

  client->rfb_client->clientGoneHook = vino_server_handle_client_gone;

  client->io_channel = g_io_channel_unix_new (rfb_client->sock);

  client->io_watch = g_io_add_watch (client->io_channel,
				     G_IO_IN|G_IO_PRI,
				     (GIOFunc) vino_server_client_data_pending,
				     rfb_client);

  server->priv->clients = g_slist_prepend (server->priv->clients, client);

  vino_server_set_client_on_hold (server, client, server->priv->on_hold);

  return server->priv->on_hold ? RFB_CLIENT_ON_HOLD : RFB_CLIENT_ACCEPT;
}

static void
vino_server_handle_prompt_response (VinoServer         *server,
				    rfbClientPtr        rfb_client,
				    VinoPromptResponse  response)
{
  g_return_if_fail (VINO_IS_SERVER (server));
  g_return_if_fail (rfb_client != NULL);

  switch (response)
    {
    case VINO_RESPONSE_ACCEPT:
      vino_server_set_client_on_hold (server, rfb_client->clientData, FALSE);
      break;
    case VINO_RESPONSE_REJECT:
      rfbCloseClient (rfb_client);
      rfbClientConnectionGone (rfb_client);
      break;
    case VINO_RESPONSE_INVALID:
      g_assert_not_reached ();
      break;
    }
}

static enum rfbNewClientAction
vino_server_handle_authenticated_client (rfbClientPtr rfb_client)
{
  VinoServer           *server = VINO_SERVER (rfb_client->screen->screenData);
  VinoServerClientInfo *client = (VinoServerClientInfo *) rfb_client->clientData;

  g_return_val_if_fail (VINO_IS_SERVER (server), RFB_CLIENT_REFUSE);

  if (!server->priv->prompt_enabled)
    return RFB_CLIENT_ACCEPT;

  vino_prompt_add_client (server->priv->prompt, rfb_client);

  vino_server_set_client_on_hold (server, client, TRUE);

  return RFB_CLIENT_ON_HOLD;
}

static gboolean
vino_server_new_connection_pending (GIOChannel   *source,
				    GIOCondition  condition,
				    VinoServer   *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), FALSE);

  rfbProcessNewConnection (server->priv->rfb_screen);

  return TRUE;
}

static void
vino_server_handle_key_event (rfbBool      down,
			      rfbKeySym    keySym,
			      rfbClientPtr rfb_client)
{
  VinoServer *server = VINO_SERVER (rfb_client->screen->screenData);

  g_return_if_fail (VINO_IS_SERVER (server));

  if (server->priv->view_only)
    return;

  vino_input_handle_key_event (server->priv->screen, keySym, down);
}

static void
vino_server_handle_pointer_event (int          buttonMask,
				  int          x,
				  int          y,
				  rfbClientPtr rfb_client)
{
  VinoServer *server = VINO_SERVER (rfb_client->screen->screenData);

  g_return_if_fail (VINO_IS_SERVER (server));

  if (server->priv->view_only)
    return;

  vino_input_handle_pointer_event (server->priv->screen, buttonMask, x, y);
}

static void
vino_server_handle_clipboard_event (char         *str,
				    int           len,
				    rfbClientPtr  rfb_client)
{
  VinoServer *server = VINO_SERVER (rfb_client->screen->screenData);

  g_return_if_fail (VINO_IS_SERVER (server));

  if (server->priv->view_only)
    return;

  vino_input_handle_clipboard_event (server->priv->screen, str, len);
}

static gboolean
vino_server_auth_client_deferred (VinoServerClientInfo *client)
{
  VinoServer              *server = VINO_SERVER (client->rfb_client->screen->screenData);
  enum rfbNewClientAction  result;

  result = vino_server_auth_client (server,
				    client,
				    client->auth_response,
				    client->auth_resp_len);

  if (result == RFB_CLIENT_ACCEPT)
    vino_server_set_client_on_hold (server, client, FALSE);

  rfbAuthPasswordChecked (client->rfb_client, result);

  g_free (client->auth_response);
  client->auth_response = NULL;
  client->auth_resp_len = 0;
  client->auth_timeout  = 0;

  return FALSE;
}

static void
vino_server_defer_client_auth (VinoServer           *server,
			       VinoServerClientInfo *client,
			       const char           *response,
			       int                   length)
{
  client->auth_resp_len = length;
  client->auth_response = g_new (char, length);
  memcpy (client->auth_response, response, length);

  vino_server_set_client_on_hold (server, client, TRUE);

  client->auth_timeout = g_timeout_add (VINO_SERVER_AUTH_DEFER_LEN * 1000,
					(GSourceFunc) vino_server_auth_client_deferred,
					client);
}

static enum rfbNewClientAction
vino_server_auth_client (VinoServer           *server,
			 VinoServerClientInfo *client,
			 const char           *response,
			 int                   length)
{
  rfbClientPtr  rfb_client;
  char         *password;

  if (!(server->priv->auth_methods & VINO_AUTH_VNC))
    goto auth_failed;

  if (!server->priv->vnc_password)
    goto auth_failed;

  if (!(password = vino_base64_unencode (server->priv->vnc_password)))
    {
      g_warning ("Failed to base64 unencode VNC password\n");
      goto auth_failed;
    }

  rfb_client = client->rfb_client;

  vncEncryptBytes (client->rfb_client->authChallenge, password);

  memset (password, 0, strlen (password));
  g_free (password);

  if (memcmp (rfb_client->authChallenge, response, length))
    {
      memset (rfb_client->authChallenge, 0, CHALLENGESIZE);
      g_warning ("VNC authentication failure from '%s'\n", rfb_client->host);
      goto auth_failed;
    }

  memset (rfb_client->authChallenge, 0, CHALLENGESIZE);

  server->priv->last_auth_failed = FALSE;

  return RFB_CLIENT_ACCEPT;

 auth_failed:
  /* Delay the next authentication attempt so as to make
   * brute force guessing of the password infeasible.
   */
  server->priv->last_auth_failed = TRUE;
  return RFB_CLIENT_REFUSE;
}

static enum rfbNewClientAction
vino_server_check_vnc_password (rfbClientPtr  rfb_client,
				const char   *response,
				int           length)
{
  VinoServer           *server = VINO_SERVER (rfb_client->screen->screenData);
  VinoServerClientInfo *client = (VinoServerClientInfo *) rfb_client->clientData;

  g_return_val_if_fail (VINO_IS_SERVER (server), FALSE);

  if (!response || length != CHALLENGESIZE)
    {
      server->priv->last_auth_failed = TRUE;
      return RFB_CLIENT_REFUSE;
    }

  if (server->priv->last_auth_failed)
    {
      g_warning ("Deferring authentication of '%s' for %d seconds\n",
		 rfb_client->host, VINO_SERVER_AUTH_DEFER_LEN);
      vino_server_defer_client_auth (server, client, response, length);
      return RFB_CLIENT_ON_HOLD;
    }

  return vino_server_auth_client (server, client, response, length);
}

static void
vino_server_handle_damage_notify (VinoServer *server)
{
  GdkRectangle *rects;
  int           i, n_rects;

  g_return_if_fail (VINO_IS_SERVER (server));
  
  rects = vino_fb_get_damage (server->priv->fb, &n_rects, TRUE);

  for (i = 0; i < n_rects; i++)
    rfbMarkRectAsModified (server->priv->rfb_screen,
			   rects [i].x,
			   rects [i].y,
			   rects [i].x + rects [i].width,
			   rects [i].y + rects [i].height);

  g_free (rects);
}

static void
vino_server_init_pixel_format (VinoServer       *server,
			       rfbScreenInfoPtr  rfb_screen)

{
  rfbPixelFormat *format = &rfb_screen->rfbServerFormat;
  gulong          red_mask, green_mask, blue_mask;

  rfb_screen->bitsPerPixel       = vino_fb_get_bits_per_pixel (server->priv->fb);
  rfb_screen->depth              = vino_fb_get_depth (server->priv->fb);
  rfb_screen->paddedWidthInBytes = vino_fb_get_rowstride (server->priv->fb);

  format->bitsPerPixel = rfb_screen->bitsPerPixel;
  format->depth        = rfb_screen->depth;

  vino_fb_get_color_masks (server->priv->fb,
			   &red_mask, &green_mask, &blue_mask);
  
  format->redShift = 0;
  while (!(red_mask & (1 << format->redShift)))
    format->redShift++;
  
  format->greenShift = 0;
  while (!(green_mask & (1 << format->greenShift)))
    format->greenShift++;

  format->blueShift = 0;
  while (!(blue_mask & (1 << format->blueShift)))
    format->blueShift++;

  format->redMax   = red_mask   >> format->redShift;
  format->greenMax = green_mask >> format->greenShift;
  format->blueMax  = blue_mask  >> format->blueShift;

  dprintf (RFB,
	   "Initialized pixel format: %dbpp, depth = %d\n"
	   "\tred:   mask = %.8lx, max = %d, shift = %d\n"
	   "\tgreen: mask = %.8lx, max = %d, shift = %d\n"
	   "\tblue:  mask = %.8lx, max = %d, shift = %d\n",
	   format->bitsPerPixel, format->depth,
	   red_mask,   format->redMax,   format->redShift,
	   green_mask, format->greenMax, format->greenShift,
	   blue_mask,  format->blueMax,  format->blueShift);
}

static void
vino_server_screen_size_changed (VinoServer *server)
{
  g_return_if_fail (VINO_IS_SERVER (server));

  rfbNewFramebuffer (server->priv->rfb_screen,
		     vino_fb_get_pixels (server->priv->fb),
		     gdk_screen_get_width (server->priv->screen),
		     gdk_screen_get_height (server->priv->screen),
		     -1, -1, -1);

  vino_server_init_pixel_format (server, server->priv->rfb_screen);
}

static void
vino_server_setup_framebuffer (VinoServer *server)
{
  g_return_if_fail (server->priv->fb == NULL);
  g_return_if_fail (server->priv->cursor_data == NULL);

  server->priv->fb = vino_fb_new (server->priv->screen);

  g_signal_connect_swapped (server->priv->fb, "size-changed",
			    G_CALLBACK (vino_server_screen_size_changed),
			    server);
  g_signal_connect_swapped (server->priv->fb, "damage-notify",
			    G_CALLBACK (vino_server_handle_damage_notify),
			    server);

  server->priv->rfb_screen->frameBuffer = vino_fb_get_pixels (server->priv->fb);
  
  vino_server_init_pixel_format (server, server->priv->rfb_screen);

  server->priv->cursor_data = vino_cursor_init (server->priv->screen);
}

static void
vino_server_release_framebuffer (VinoServer *server)
{
  g_return_if_fail (server->priv->fb != NULL);
  g_return_if_fail (server->priv->cursor_data != NULL);

  if (server->priv->cursor_data)
    vino_cursor_finalize (server->priv->cursor_data);
  server->priv->cursor_data = NULL;

  server->priv->rfb_screen->frameBuffer = NULL;

  g_object_unref (server->priv->fb);
  server->priv->fb = NULL;
}

static void
vino_server_init_from_screen (VinoServer *server,
			      GdkScreen  *screen)
{
  rfbScreenInfoPtr rfb_screen;

  g_return_if_fail (server->priv->screen == NULL);
  g_return_if_fail (screen != NULL);
  
  server->priv->screen = screen;

  server->priv->prompt = vino_prompt_new (screen);

  g_signal_connect_swapped (server->priv->prompt, "response",
			    G_CALLBACK (vino_server_handle_prompt_response),
			    server);

  /* libvncserver NOTE:
   *   we don't pass in argc or argv
   *   samplesPerPixel is totally unused (3 below)
   *   bitsPerSample and bytesPerPixel get set in
   *   set_pixel_format()
   */
  server->priv->rfb_screen = rfb_screen = 
    rfbGetScreen (NULL, NULL,
		  gdk_screen_get_width  (screen),
		  gdk_screen_get_height (screen),
		  -1, -1, -1);

  /* libvncserver NOTE:
   *   DeferUpdateTime is the number of milliseconds to wait
   *   before actually responding to a frame buffer update
   *   request.
   *   setting autoPort enables autoProbing a port between
   *   5900-6000
   */
  rfb_screen->rfbDeferUpdateTime = 0;
  rfb_screen->autoPort           = TRUE;
  rfb_screen->rfbAlwaysShared    = TRUE;

  rfbInitServer (rfb_screen);
  
  /* libvncserver NOTE:
   *   there's no user_data for newClientHook. You have
   *   to use screenData
   */
  rfb_screen->screenData              = server;
  rfb_screen->newClientHook           = vino_server_handle_new_client;
  rfb_screen->authenticatedClientHook = vino_server_handle_authenticated_client;

  /* libvncserver NOTE:
   *   all these hooks should take an rfbClientPtr as the
   *   first argument rather than the last.
   */
  rfb_screen->kbdAddEvent = vino_server_handle_key_event;
  rfb_screen->ptrAddEvent = vino_server_handle_pointer_event;
  rfb_screen->setXCutText = vino_server_handle_clipboard_event;

  rfb_screen->passwordCheck = vino_server_check_vnc_password;

  dprintf (RFB, "Creating watch for listening socket %d - port %d\n",
	   rfb_screen->rfbListenSock, rfb_screen->rfbPort);

  server->priv->io_channel = g_io_channel_unix_new (rfb_screen->rfbListenSock);
  server->priv->io_watch =
    g_io_add_watch (server->priv->io_channel,
		    G_IO_IN|G_IO_PRI,
		    (GIOFunc) vino_server_new_connection_pending,
		    server);

#ifdef VINO_ENABLE_HTTP_SERVER
  server->priv->http = vino_http_get (rfb_screen->rfbPort);
#endif
}

static void
vino_server_finalize (GObject *object)
{
  VinoServer *server = VINO_SERVER (object);

#ifdef VINO_ENABLE_HTTP_SERVER
  if (server->priv->http)
    {
      vino_http_remove_rfb_port (server->priv->http,
				 server->priv->rfb_screen->rfbPort);
      g_object_unref (server->priv->http);
    }
  server->priv->http = NULL;
#endif /* VINO_ENABLE_HTTP_SERVER */

  if (server->priv->io_watch)
    g_source_remove (server->priv->io_watch);
  server->priv->io_watch = 0;

  if (server->priv->io_channel)
    g_io_channel_unref (server->priv->io_channel);
  server->priv->io_channel = NULL;
  
  if (server->priv->rfb_screen)
    rfbScreenCleanup (server->priv->rfb_screen);
  server->priv->rfb_screen = NULL;

  /* ClientGoneHook should get invoked for each
   * client from rfbScreenCleanup()
   */
  g_assert (server->priv->clients == NULL);

  if (server->priv->vnc_password)
    g_free (server->priv->vnc_password);
  server->priv->vnc_password = NULL;

  if (server->priv->prompt)
    g_object_unref (server->priv->prompt);
  server->priv->prompt = NULL;

  if (server->priv->cursor_data)
    vino_cursor_finalize (server->priv->cursor_data);
  server->priv->cursor_data = NULL;

  if (server->priv->fb)
    g_object_unref (server->priv->fb);
  server->priv->fb = NULL;
  
  g_free (server->priv);
  server->priv = NULL;

  if (G_OBJECT_CLASS (parent_class)->finalize)
    G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
vino_server_set_property (GObject      *object,
			  guint         prop_id,
			  const GValue *value,
			  GParamSpec   *pspec)
{
  VinoServer *server = VINO_SERVER (object);

  switch (prop_id)
    {
    case PROP_SCREEN:
      vino_server_init_from_screen (server, g_value_get_object (value));
      break;
    case PROP_ON_HOLD:
      vino_server_set_on_hold (server, g_value_get_boolean (value));
      break;
    case PROP_PROMPT_ENABLED:
      vino_server_set_prompt_enabled (server, g_value_get_boolean (value));
      break;
    case PROP_VIEW_ONLY:
      vino_server_set_view_only (server, g_value_get_boolean (value));
      break;
    case PROP_REQUIRE_ENCRYPTION:
      vino_server_set_require_encryption (server, g_value_get_boolean (value));
      break;
    case PROP_AUTH_METHODS:
      vino_server_set_auth_methods (server, g_value_get_flags (value));
      break;
    case PROP_VNC_PASSWORD:
      vino_server_set_vnc_password (server, g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
vino_server_get_property (GObject    *object,
			  guint       prop_id,
			  GValue     *value,
			  GParamSpec *pspec)
{
  VinoServer *server = VINO_SERVER (object);

  switch (prop_id)
    {
    case PROP_SCREEN:
      g_value_set_object (value, server->priv->screen);
      break;
    case PROP_ON_HOLD:
      g_value_set_boolean (value, server->priv->on_hold);
      break;
    case PROP_PROMPT_ENABLED:
      g_value_set_boolean (value, server->priv->prompt_enabled);
      break;
    case PROP_VIEW_ONLY:
      g_value_set_boolean (value, server->priv->view_only);
      break;
    case PROP_REQUIRE_ENCRYPTION:
      g_value_set_boolean (value, server->priv->view_only);
      break;
    case PROP_AUTH_METHODS:
      g_value_set_flags (value, server->priv->auth_methods);
      break;
    case PROP_VNC_PASSWORD:
      g_value_set_string (value, server->priv->vnc_password);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
vino_server_instance_init (VinoServer *server)
{
  server->priv = g_new0 (VinoServerPrivate, 1);
}

static void
vino_server_class_init (VinoServerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  gobject_class->finalize     = vino_server_finalize;
  gobject_class->set_property = vino_server_set_property;
  gobject_class->get_property = vino_server_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_SCREEN,
				   g_param_spec_object ("screen",
							_("Screen"),
							_("The screen for which to create a VNC server"),
							GDK_TYPE_SCREEN,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  
  g_object_class_install_property (gobject_class,
				   PROP_ON_HOLD,
				   g_param_spec_boolean ("on-hold",
							 _("On Hold"),
							 _("Place all clients on hold"),
							 TRUE,
							 G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class,
				   PROP_PROMPT_ENABLED,
				   g_param_spec_boolean ("prompt-enabled",
							 _("Prompt enabled"),
							 _("Prompt the user about connection attempts"),
							 TRUE,
							 G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class,
				   PROP_VIEW_ONLY,
				   g_param_spec_boolean ("view-only",
							 _("View Only"),
							 _("Disallow keyboard/pointer input from clients"),
							 FALSE,
							 G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class,
				   PROP_REQUIRE_ENCRYPTION,
				   g_param_spec_boolean ("require-encryption",
							 _("Require Encryption"),
							 _("Require clients to use encryption"),
							 TRUE,
							 G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class,
				   PROP_AUTH_METHODS,
				   g_param_spec_flags ("auth-methods",
						       _("Authentication methods"),
						       _("The authentication methods this server should allow"),
						       VINO_TYPE_AUTH_METHOD,
						       VINO_AUTH_NONE,
						       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  
  g_object_class_install_property (gobject_class,
				   PROP_VNC_PASSWORD,
				   g_param_spec_string ("vnc-password",
							_("VNC Password"),
							_("The password (base64 encoded) used to authenticate types using the VncAuth method"),
							NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
}

GType
vino_server_get_type (void)
{
  static GType object_type = 0;

  if (!object_type)
    {
      static const GTypeInfo object_info =
	{
	  sizeof (VinoServerClass),
	  (GBaseInitFunc) NULL,
	  (GBaseFinalizeFunc) NULL,
	  (GClassInitFunc) vino_server_class_init,
	  NULL,           /* class_finalize */
	  NULL,           /* class_data */
	  sizeof (VinoServer),
	  0,              /* n_preallocs */
	  (GInstanceInitFunc) vino_server_instance_init,
	};
      
      object_type = g_type_register_static (G_TYPE_OBJECT,
                                            "VinoServer",
                                            &object_info, 0);
    }

  return object_type;
}


VinoServer *
vino_server_new (GdkScreen *screen,
		 gboolean   view_only)
{
  g_return_val_if_fail (GDK_IS_SCREEN (screen), NULL);

  return g_object_new (VINO_TYPE_SERVER,
		       "screen", screen,
		       "view-only", view_only,
		       NULL);
}

GdkScreen *
vino_server_get_screen (VinoServer *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), NULL);

  return server->priv->screen;
}

gboolean
vino_server_get_view_only (VinoServer *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), FALSE);

  return server->priv->view_only;
}

void
vino_server_set_view_only (VinoServer *server,
			   gboolean    view_only)
{
  g_return_if_fail (VINO_IS_SERVER (server));

  view_only = view_only != FALSE;

  if (server->priv->view_only != view_only)
    {
      server->priv->view_only = view_only;

      g_object_notify (G_OBJECT (server), "view-only");
    }
}

gboolean
vino_server_get_on_hold (VinoServer *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), FALSE);

  return server->priv->on_hold;
}

void
vino_server_set_on_hold (VinoServer *server,
			 gboolean    on_hold)
{
  g_return_if_fail (VINO_IS_SERVER (server));

  on_hold = on_hold != FALSE;

  if (server->priv->on_hold != on_hold)
    {
      GSList *l;

      server->priv->on_hold = on_hold;

      for (l = server->priv->clients; l; l = l->next)
	{
	  VinoServerClientInfo *client = l->data;

	  /* If a client is on hold before we have initialized,
	   * we want to leave it on hold until the deferred
	   * authentication or the prompt has completed.
	   *
	   * FIXME: this isn't correct, though - e.g if we're
	   * in the middle of prompting and you toggle the
	   * server on and off then the client gets through.
	   */
	  if (client->rfb_client->state == RFB_NORMAL)
	    vino_server_set_client_on_hold (server, client, on_hold);
	}

      g_object_notify (G_OBJECT (server), "on-hold");
    }
}

gboolean
vino_server_get_prompt_enabled (VinoServer *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), FALSE);

  return server->priv->prompt_enabled;
}

void
vino_server_set_prompt_enabled (VinoServer *server,
				gboolean    prompt_enabled)
{
  g_return_if_fail (VINO_IS_SERVER (server));

  prompt_enabled = prompt_enabled != FALSE;

  if (server->priv->prompt_enabled != prompt_enabled)
    {
      server->priv->prompt_enabled = prompt_enabled;

      g_object_notify (G_OBJECT (server), "prompt-enabled");
    }
}

static void
vino_server_update_security_types (VinoServer *server)
{
  rfbClearSecurityTypes (server->priv->rfb_screen);
  rfbClearAuthTypes (server->priv->rfb_screen);

#ifdef HAVE_GNUTLS
  rfbAddSecurityType (server->priv->rfb_screen, rfbTLS);
#endif
      
  if (server->priv->auth_methods & VINO_AUTH_VNC)
    {
      rfbAddAuthType (server->priv->rfb_screen, rfbVncAuth);
#ifdef HAVE_GNUTLS
      if (!server->priv->require_encryption)
#endif
	rfbAddSecurityType (server->priv->rfb_screen, rfbVncAuth);
    }
      
  if (server->priv->auth_methods & VINO_AUTH_NONE)
    {
      rfbAddAuthType (server->priv->rfb_screen, rfbNoAuth);
#ifdef HAVE_GNUTLS
      if (!server->priv->require_encryption)
#endif
	rfbAddSecurityType (server->priv->rfb_screen, rfbNoAuth);
    }
}
void
vino_server_set_require_encryption (VinoServer *server,
				    gboolean    require_encryption)
{
  g_return_if_fail (VINO_IS_SERVER (server));
  
  require_encryption = require_encryption != FALSE;

  if (server->priv->require_encryption != require_encryption)
    {
      server->priv->require_encryption = require_encryption;

      vino_server_update_security_types (server);

      g_object_notify (G_OBJECT (server), "require-encryption");
    }
}

gboolean
vino_server_get_require_encryption (VinoServer *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), FALSE);

  return server->priv->require_encryption;
}

void
vino_server_set_auth_methods (VinoServer     *server,
			      VinoAuthMethod  auth_methods)
{
  g_return_if_fail (VINO_IS_SERVER (server));
  g_return_if_fail (auth_methods != VINO_AUTH_INVALID);

  if (server->priv->auth_methods != auth_methods)
    {
      server->priv->auth_methods = auth_methods;

      vino_server_update_security_types (server);

      g_object_notify (G_OBJECT (server), "auth-methods");
    }
}

VinoAuthMethod
vino_server_get_auth_methods (VinoServer *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), VINO_AUTH_INVALID);

  return server->priv->auth_methods;
}

void
vino_server_set_vnc_password (VinoServer *server,
			      const char *vnc_password)
{
  g_return_if_fail (VINO_IS_SERVER (server));

  if (server->priv->vnc_password)
    g_free (server->priv->vnc_password);

  server->priv->vnc_password = g_strdup (vnc_password);

  g_object_notify (G_OBJECT (server), "vnc-password");
}

G_CONST_RETURN char *
vino_server_get_vnc_password (VinoServer *server)
{
  g_return_val_if_fail (VINO_IS_SERVER (server), NULL);

  return server->priv->vnc_password;
}
