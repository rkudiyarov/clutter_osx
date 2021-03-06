/* Clutter.
 * An OpenGL based 'interactive canvas' library.
 * Authored By Matthew Allum  <mallum@openedhand.com>
 * Copyright (C) 2006-2007 OpenedHand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-stage-egl.h"
#include "clutter-backend-egl.h"
#include "clutter-egl.h"

#include "../clutter-backend.h"
#include "../clutter-event.h"
#include "../clutter-private.h"
#include "../clutter-debug.h"
#include "../clutter-main.h"

#include <string.h>

#include <glib.h>

#ifdef HAVE_TSLIB
#include <tslib.h>
#endif

typedef struct _ClutterEventSource  ClutterEventSource;

struct _ClutterEventSource
{
  GSource source;

  ClutterBackendEGL *backend;
  GPollFD event_poll_fd;

#ifdef HAVE_TSLIB
  struct tsdev   *ts_device;
#endif
};

#ifdef HAVE_TSLIB

static gboolean clutter_event_prepare  (GSource     *source,
                                        gint        *timeout);
static gboolean clutter_event_check    (GSource     *source);
static gboolean clutter_event_dispatch (GSource     *source,
                                        GSourceFunc  callback,
                                        gpointer     user_data);

static GList *event_sources = NULL;

static GSourceFuncs event_funcs = {
  clutter_event_prepare,
  clutter_event_check,
  clutter_event_dispatch,
  NULL
};

static GSource *
clutter_event_source_new (ClutterBackendEGL *backend)
{
  GSource *source = g_source_new (&event_funcs, sizeof (ClutterEventSource));
  ClutterEventSource *event_source = (ClutterEventSource *) source;

  event_source->backend = backend;

  return source;
}

static guint32
get_backend_time (void)
{
  ClutterBackendEGL *backend_egl;

  backend_egl = CLUTTER_BACKEND_EGL (clutter_get_default_backend ());

  return g_timer_elapsed (backend_egl->event_timer, NULL) * 1000;
}
#endif

void
_clutter_events_egl_init (ClutterBackendEGL *backend_egl)
{
#ifdef HAVE_TSLIB
  ClutterEventSource *event_source;
  const char *device_name;
  GSource *source;

  CLUTTER_NOTE (EVENT, "Starting timer");
  g_assert (backend_egl->event_timer != NULL);
  g_timer_start (backend_egl->event_timer);

  source = backend_egl->event_source = clutter_event_source_new (backend_egl);
  event_source = (ClutterEventSource *) source;

  device_name = g_getenv ("TSLIB_TSDEVICE");
  if (device_name == NULL || device_name[0] == '\0')
    {
      g_warning ("No device for TSLib has been defined; please set the "
                 "TSLIB_TSDEVICE environment variable to define a touch "
                 "screen device to be used with Clutter.");
      g_source_unref (source);
      return;
    }

  event_source->ts_device = ts_open (device_name, 0);
  if (event_source->ts_device)
    {
      CLUTTER_NOTE (EVENT, "Opened '%s'", device_name);

      if (ts_config (event_source->ts_device))
	{
	  g_warning ("Closing device '%s': ts_config() failed", device_name);
	  ts_close (event_source->ts_device);
          g_source_unref (source);
	  return;
	}

      g_source_set_priority (source, CLUTTER_PRIORITY_EVENTS);
      event_source->event_poll_fd.fd = ts_fd (event_source->ts_device);
      event_source->event_poll_fd.events = G_IO_IN;

      event_sources = g_list_prepend (event_sources, event_source);

      g_source_add_poll (source, &event_source->event_poll_fd);
      g_source_set_can_recurse (source, TRUE);
      g_source_attach (source, NULL);
    }
  else
    {
      g_warning ("Unable to open '%s'", device_name);
      g_source_unref (source);
    }
#endif /* HAVE_TSLIB */
}

void
_clutter_events_egl_uninit (ClutterBackendEGL *backend_egl)
{
#ifdef HAVE_TSLIB
  if (backend_egl->event_timer != NULL)
    {
      CLUTTER_NOTE (EVENT, "Stopping the timer");
      g_timer_stop (backend_egl->event_timer);
    }

  if (backend_egl->event_source != NULL)
    {
      CLUTTER_NOTE (EVENT, "Destroying the event source");

      ClutterEventSource *event_source =
                (ClutterEventSource *) backend_egl->event_source;

      ts_close (event_source->ts_device);
      event_sources = g_list_remove (event_sources, backend_egl->event_source);

      g_source_destroy (backend_egl->event_source);
      g_source_unref (backend_egl->event_source);
      backend_egl->event_source = NULL;
    }
#endif /* HAVE_TSLIB */
}

#ifdef HAVE_TSLIB

static gboolean
clutter_event_prepare (GSource *source,
                       gint    *timeout)
{
  gboolean retval;

  clutter_threads_enter ();

  *timeout = -1;
  retval = clutter_events_pending ();

  clutter_threads_leave ();

  return retval;
}

static gboolean
clutter_event_check (GSource *source)
{
  ClutterEventSource *event_source = (ClutterEventSource *) source;
  gboolean retval;

  clutter_threads_enter ();

  retval = ((event_source->event_poll_fd.revents & G_IO_IN) ||
            clutter_events_pending ());

  clutter_threads_leave ();

  return retval;
}

static gboolean
clutter_event_dispatch (GSource     *source,
                        GSourceFunc  callback,
                        gpointer     user_data)
{
  ClutterEvent *event;
  ClutterEventSource *event_source = (ClutterEventSource *) source;
  struct ts_sample    tsevent;
  ClutterMainContext *clutter_context;

  clutter_threads_enter ();

  clutter_context = _clutter_context_get_default ();

  /* FIXME while would be better here but need to deal with lockups */
  if ((!clutter_events_pending()) &&
      (ts_read(event_source->ts_device, &tsevent, 1) == 1))
    {
      static gint     last_x = 0, last_y = 0;
      static gboolean clicked = FALSE;

      /* Avoid sending too many events which are just pressure changes.
       *
       * FIXME - We don't current handle pressure in events and thus
       * event_button_generate gets confused generating lots of double
       * and triple clicks.
      */
      if (tsevent.pressure && last_x == tsevent.x && last_y == tsevent.y)
        goto out;

      event = clutter_event_new (CLUTTER_NOTHING);

      event->any.stage = clutter_stage_get_default ();

      last_x = event->button.x = tsevent.x;
      last_y = event->button.y = tsevent.y;

      if (tsevent.pressure && !clicked)
        {
	  event->button.type = event->type = CLUTTER_BUTTON_PRESS;
          event->button.time = get_backend_time ();
          event->button.modifier_state = 0;
          event->button.button = 1;

          clicked = TRUE;
        }
      else if (tsevent.pressure && clicked)
        {
          event->motion.type = event->type = CLUTTER_MOTION;
          event->motion.time = get_backend_time ();
          event->motion.modifier_state = 0;
        }
      else
        {
	  event->button.type = event->type = CLUTTER_BUTTON_RELEASE;
          event->button.time = get_backend_time ();
          event->button.modifier_state = 0;
          event->button.button = 1;

          clicked = FALSE;
        }

      g_queue_push_head (clutter_context->events_queue, event);
    }

  /* Pop an event off the queue if any */
  event = clutter_event_get ();

  if (event)
    {
      /* forward the event into clutter for emission etc. */
      clutter_do_event (event);
      clutter_event_free (event);
    }

out:

  clutter_threads_leave ();

  return TRUE;
}

#endif
