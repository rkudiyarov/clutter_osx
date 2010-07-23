/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2006 OpenedHand
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
/**
 * SECTION:clutter-main
 * @short_description: Various 'global' clutter functions.
 *
 * Functions to retrieve various global Clutter resources and other utility
 * functions for mainloops, events and threads
 *
 * <refsect2 id="clutter-Threading-Model">
 *   <title>Threading Model</title>
 *   <para>Clutter is <emphasis>thread-aware</emphasis>: all operations
 *   performed by Clutter are assumed to be under the big Clutter lock,
 *   which is created when the threading is initialized through
 *   clutter_threads_init().</para>
 *   <example id="example-Thread-Init">
 *     <title>Thread Initialization</title>
 *     <para>The code below shows how to correctly initialize Clutter
 *     in a multi-threaded environment. These operations are mandatory for
 *     applications that wish to use threads with Clutter.</para>
 *     <programlisting>
 * int
 * main (int argc, char *argv[])
 * {
 *   /&ast; initialize GLib's threading support &ast;/
 *   g_thread_init (NULL);
 *
 *   /&ast; initialize Clutter's threading support &ast;/
 *   clutter_threads_init ();
 *
 *   /&ast; initialize Clutter &ast;/
 *   clutter_init (&amp;argc, &amp;argv);
 *
 *   /&ast; program code &ast;/
 *
 *   /&ast; acquire the main lock &ast;/
 *   clutter_threads_enter ();
 *
 *   /&ast; start the main loop &ast;/
 *   clutter_main ();
 *
 *   /&ast; release the main lock &ast;/
 *   clutter_threads_leave ();
 *
 *   /&ast; clean up &ast;/
 *   return 0;
 * }
 *     </programlisting>
 *   </example>
 *   <para>This threading model has the caveat that it is only safe to call
 *   Clutter's API when the lock has been acquired &mdash; which happens
 *   between pairs of clutter_threads_enter() and clutter_threads_leave()
 *   calls.</para>
 *   <para>The only safe and portable way to use the Clutter API in a
 *   multi-threaded environment is to never access the API from a thread that
 *   did not call clutter_init() and clutter_main().</para>
 *   <para>The common pattern for using threads with Clutter is to use worker
 *   threads to perform blocking operations and then install idle or timeour
 *   sources with the result when the thread finished.</para>
 *   <para>Clutter provides thread-aware variants of g_idle_add() and
 *   g_timeout_add() that acquire the Clutter lock before invoking the provided
 *   callback: clutter_threads_add_idle() and
 *   clutter_threads_add_timeout().</para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <glib/gi18n-lib.h>
#include <locale.h>

#ifdef USE_GDKPIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include "clutter-event.h"
#include "clutter-backend.h"
#include "clutter-main.h"
#include "clutter-master-clock.h"
#include "clutter-feature.h"
#include "clutter-actor.h"
#include "clutter-stage.h"
#include "clutter-private.h"
#include "clutter-debug.h"
#include "clutter-version.h" 	/* For flavour define */
#include "clutter-frame-source.h"
#include "clutter-profile.h"

#include "cogl/cogl.h"
#include "pango/cogl-pango.h"

#include "cally.h" /* For accessibility support */

/* main context */
static ClutterMainContext *ClutterCntx       = NULL;

/* main lock and locking/unlocking functions */
static GMutex *clutter_threads_mutex         = NULL;
static GCallback clutter_threads_lock        = NULL;
static GCallback clutter_threads_unlock      = NULL;

/* command line options */
static gboolean clutter_is_initialized       = FALSE;
static gboolean clutter_show_fps             = FALSE;
static gboolean clutter_fatal_warnings       = FALSE;
static gboolean clutter_disable_mipmap_text  = FALSE;
static gboolean clutter_use_fuzzy_picking    = FALSE;
static gboolean clutter_enable_accessibility = TRUE;

static guint clutter_default_fps             = 60;

static PangoDirection clutter_text_direction = CLUTTER_TEXT_DIRECTION_LTR;

static guint clutter_main_loop_level         = 0;
static GSList *main_loops                    = NULL;

guint clutter_debug_flags = 0;  /* global clutter debug flag */
guint clutter_paint_debug_flags = 0;
guint clutter_pick_debug_flags = 0;

guint clutter_profile_flags = 0;  /* global clutter profile flag */

const guint clutter_major_version = CLUTTER_MAJOR_VERSION;
const guint clutter_minor_version = CLUTTER_MINOR_VERSION;
const guint clutter_micro_version = CLUTTER_MICRO_VERSION;

#ifdef CLUTTER_ENABLE_DEBUG
static const GDebugKey clutter_debug_keys[] = {
  { "misc", CLUTTER_DEBUG_MISC },
  { "actor", CLUTTER_DEBUG_ACTOR },
  { "texture", CLUTTER_DEBUG_TEXTURE },
  { "event", CLUTTER_DEBUG_EVENT },
  { "paint", CLUTTER_DEBUG_PAINT },
  { "gl", CLUTTER_DEBUG_GL },
  { "alpha", CLUTTER_DEBUG_ALPHA },
  { "behaviour", CLUTTER_DEBUG_BEHAVIOUR },
  { "pango", CLUTTER_DEBUG_PANGO },
  { "backend", CLUTTER_DEBUG_BACKEND },
  { "scheduler", CLUTTER_DEBUG_SCHEDULER },
  { "script", CLUTTER_DEBUG_SCRIPT },
  { "shader", CLUTTER_DEBUG_SHADER },
  { "multistage", CLUTTER_DEBUG_MULTISTAGE },
  { "animation", CLUTTER_DEBUG_ANIMATION },
  { "layout", CLUTTER_DEBUG_LAYOUT }
};
#endif /* CLUTTER_ENABLE_DEBUG */

static const GDebugKey clutter_pick_debug_keys[] = {
  { "nop-picking", CLUTTER_DEBUG_NOP_PICKING },
  { "dump-pick-buffers", CLUTTER_DEBUG_DUMP_PICK_BUFFERS }
};

static const GDebugKey clutter_paint_debug_keys[] = {
  { "disable-swap-events", CLUTTER_DEBUG_DISABLE_SWAP_EVENTS },
  { "disable-clipped-redraws", CLUTTER_DEBUG_DISABLE_CLIPPED_REDRAWS },
  { "redraws", CLUTTER_DEBUG_REDRAWS }
};

#ifdef CLUTTER_ENABLE_PROFILE
static const GDebugKey clutter_profile_keys[] = {
  {"picking-only", CLUTTER_PROFILE_PICKING_ONLY },
  {"disable-report", CLUTTER_PROFILE_DISABLE_REPORT }
};
#endif /* CLUTTER_ENABLE_DEBUG */

/**
 * clutter_get_show_fps:
 *
 * Returns whether Clutter should print out the frames per second on the
 * console. You can enable this setting either using the
 * <literal>CLUTTER_SHOW_FPS</literal> environment variable or passing
 * the <literal>--clutter-show-fps</literal> command line argument. *
 *
 * Return value: %TRUE if Clutter should show the FPS.
 *
 * Since: 0.4
 */
gboolean
clutter_get_show_fps (void)
{
  return clutter_show_fps;
}

/**
 * clutter_get_accessibility_enabled:
 *
 * Returns whether Clutter has accessibility support enabled.  As
 * least, a value of TRUE means that there are a proper AtkUtil
 * implementation available
 *
 * Return value: %TRUE if Clutter has accessibility support enabled
 *
 * Since: 1.4
 */
gboolean
clutter_get_accessibility_enabled (void)
{
  return cally_get_cally_initialized ();
}


void
_clutter_stage_maybe_relayout (ClutterActor *stage)
{
  gfloat natural_width, natural_height;
  ClutterActorBox box = { 0, };
  CLUTTER_STATIC_TIMER (relayout_timer,
                        "Mainloop", /* no parent */
                        "Layouting",
                        "The time spent reallocating the stage",
                        0 /* no application private data */);

  /* avoid reentrancy */
  if (!CLUTTER_ACTOR_IN_RELAYOUT (stage))
    {
      CLUTTER_TIMER_START (_clutter_uprof_context, relayout_timer);
      CLUTTER_NOTE (ACTOR, "Recomputing layout");

      CLUTTER_SET_PRIVATE_FLAGS (stage, CLUTTER_IN_RELAYOUT);

      natural_width = natural_height = 0;
      clutter_actor_get_preferred_size (stage,
                                        NULL, NULL,
                                        &natural_width, &natural_height);

      box.x1 = 0;
      box.y1 = 0;
      box.x2 = natural_width;
      box.y2 = natural_height;

      CLUTTER_NOTE (ACTOR, "Allocating (0, 0 - %d, %d) for the stage",
                    (int) natural_width,
                    (int) natural_height);

      clutter_actor_allocate (stage, &box, CLUTTER_ALLOCATION_NONE);

      CLUTTER_UNSET_PRIVATE_FLAGS (stage, CLUTTER_IN_RELAYOUT);
      CLUTTER_TIMER_STOP (_clutter_uprof_context, relayout_timer);
    }
}

void
_clutter_stage_maybe_setup_viewport (ClutterStage *stage)
{
  if ((CLUTTER_PRIVATE_FLAGS (stage) & CLUTTER_SYNC_MATRICES) &&
      !CLUTTER_STAGE_IN_RESIZE (stage))
    {
      ClutterPerspective perspective;
      gfloat width, height;

      clutter_actor_get_preferred_size (CLUTTER_ACTOR (stage),
                                        NULL, NULL,
                                        &width, &height);
      clutter_stage_get_perspective (stage, &perspective);

      CLUTTER_NOTE (PAINT,
                    "Setting up the viewport { w:%.2f, h:%.2f }",
                    width, height);

      _cogl_setup_viewport (width, height,
                            perspective.fovy,
                            perspective.aspect,
                            perspective.z_near,
                            perspective.z_far);

      CLUTTER_UNSET_PRIVATE_FLAGS (stage, CLUTTER_SYNC_MATRICES);
    }
}

void
_clutter_do_redraw (ClutterStage *stage)
{
  static GTimer *timer = NULL;
  static guint timer_n_frames = 0;
  ClutterMainContext *ctx;

  ctx = _clutter_context_get_default ();

  /* Before we can paint, we have to be sure we have the latest layout */
  _clutter_stage_maybe_relayout (CLUTTER_ACTOR (stage));

  _clutter_backend_ensure_context (ctx->backend, stage);

  /* Setup FPS count - not currently across *all* stages rather than per */
  if (G_UNLIKELY (clutter_get_show_fps ()))
    {
      if (!timer)
	timer = g_timer_new ();
    }

  /* The code below can't go in stage paint as base actor_paint
   * will get called before it (and break picking, etc)
   */
  _clutter_stage_maybe_setup_viewport (stage);

  /* Call through to the actual backend to do the painting down from
   * the stage. It will likely need to swap buffers, vblank sync etc
   * which will be windowing system dependent
  */
  _clutter_backend_redraw (ctx->backend, stage);

  /* Complete FPS info */
  if (G_UNLIKELY (clutter_get_show_fps ()))
    {
      timer_n_frames++;

      if (g_timer_elapsed (timer, NULL) >= 1.0)
	{
	  g_print ("*** FPS: %i ***\n", timer_n_frames);
	  timer_n_frames = 0;
	  g_timer_start (timer);
	}
    }

  CLUTTER_TIMESTAMP (SCHEDULER, "Redraw finish for stage:%p", stage);
}

/**
 * clutter_redraw:
 *
 * Forces a redraw of the entire stage. Applications should never use this
 * function, but queue a redraw using clutter_actor_queue_redraw().
 *
 * This function should only be used by libraries integrating Clutter from
 * within another toolkit.
 */
void
clutter_redraw (ClutterStage *stage)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  clutter_stage_ensure_redraw (stage);
}

/**
 * clutter_set_motion_events_enabled:
 * @enable: %TRUE to enable per-actor motion events
 *
 * Sets whether per-actor motion events should be enabled or not (the
 * default is to enable them).
 *
 * If @enable is %FALSE the following events will not work:
 * <itemizedlist>
 *   <listitem><para>ClutterActor::motion-event, unless on the
 *     #ClutterStage</para></listitem>
 *   <listitem><para>ClutterActor::enter-event</para></listitem>
 *   <listitem><para>ClutterActor::leave-event</para></listitem>
 * </itemizedlist>
 *
 * Since: 0.6
 */
void
clutter_set_motion_events_enabled (gboolean enable)
{
  ClutterMainContext *context = _clutter_context_get_default ();

  context->motion_events_per_actor = enable;
}

/**
 * clutter_get_motion_events_enabled:
 *
 * Gets whether the per-actor motion events are enabled.
 *
 * Return value: %TRUE if the motion events are enabled
 *
 * Since: 0.6
 */
gboolean
clutter_get_motion_events_enabled (void)
{
  ClutterMainContext *context = _clutter_context_get_default ();

  return context->motion_events_per_actor;
}

guint _clutter_pix_to_id (guchar pixel[4]);

void
_clutter_id_to_color (guint id, ClutterColor *col)
{
  ClutterMainContext *ctx;
  gint red, green, blue;

  ctx = _clutter_context_get_default ();

  /* compute the numbers we'll store in the components */
  red   = (id >> (ctx->fb_g_mask_used+ctx->fb_b_mask_used))
        & (0xff >> (8-ctx->fb_r_mask_used));
  green = (id >> ctx->fb_b_mask_used)
        & (0xff >> (8-ctx->fb_g_mask_used));
  blue  = (id)
        & (0xff >> (8-ctx->fb_b_mask_used));

  /* shift left bits a bit and add one, this circumvents
   * at least some potential rounding errors in GL/GLES
   * driver / hw implementation.
   */
  if (ctx->fb_r_mask_used != ctx->fb_r_mask)
    red = red * 2;
  if (ctx->fb_g_mask_used != ctx->fb_g_mask)
    green = green * 2;
  if (ctx->fb_b_mask_used != ctx->fb_b_mask)
    blue  = blue  * 2;

  /* shift up to be full 8bit values */
  red   = (red   << (8 - ctx->fb_r_mask)) | (0x7f >> (ctx->fb_r_mask_used));
  green = (green << (8 - ctx->fb_g_mask)) | (0x7f >> (ctx->fb_g_mask_used));
  blue  = (blue  << (8 - ctx->fb_b_mask)) | (0x7f >> (ctx->fb_b_mask_used));

  col->red   = red;
  col->green = green;
  col->blue  = blue;
  col->alpha = 0xff;

  /* XXX: We rotate the nibbles of the colors here so that there is a
   * visible variation between colors of sequential actor identifiers;
   * otherwise pick buffers dumped to an image will pretty much just look
   * black.
   */
  if (G_UNLIKELY (clutter_pick_debug_flags & CLUTTER_DEBUG_DUMP_PICK_BUFFERS))
    {
      col->red   = (col->red << 4)   | (col->red >> 4);
      col->green = (col->green << 4) | (col->green >> 4);
      col->blue  = (col->blue << 4)  | (col->blue >> 4);
    }
}

guint
_clutter_pixel_to_id (guchar pixel[4])
{
  ClutterMainContext *ctx;
  gint  red, green, blue;
  guint id;

  ctx = _clutter_context_get_default ();

  /* reduce the pixel components to the number of bits actually used of the
   * 8bits.
   */
  if (G_UNLIKELY (clutter_pick_debug_flags & CLUTTER_DEBUG_DUMP_PICK_BUFFERS))
    {
      guchar tmp;

      /* XXX: In _clutter_id_to_color we rotated the nibbles of the colors so
       * that there is a visible variation between colors of sequential actor
       * identifiers (otherwise pick buffers dumped to an image will pretty
       * much just look black.) Here we reverse that rotation.
       */
      tmp = ((pixel[0] << 4) | (pixel[0] >> 4));
      red = tmp >> (8 - ctx->fb_r_mask);
      tmp = ((pixel[1] << 4) | (pixel[1] >> 4));
      green = tmp >> (8 - ctx->fb_g_mask);
      tmp = ((pixel[2] << 4) | (pixel[2] >> 4));
      blue = tmp >> (8 - ctx->fb_b_mask);
    }
  else
    {
      red   = pixel[0] >> (8 - ctx->fb_r_mask);
      green = pixel[1] >> (8 - ctx->fb_g_mask);
      blue  = pixel[2] >> (8 - ctx->fb_b_mask);
    }

  /* divide potentially by two if 'fuzzy' */
  red   = red   >> (ctx->fb_r_mask - ctx->fb_r_mask_used);
  green = green >> (ctx->fb_g_mask - ctx->fb_g_mask_used);
  blue  = blue  >> (ctx->fb_b_mask - ctx->fb_b_mask_used);

  /* combine the correct per component values into the final id */
  id = blue
     + (green <<  ctx->fb_b_mask_used)
     + (red << (ctx->fb_b_mask_used + ctx->fb_g_mask_used));

  return id;
}

#ifdef USE_GDKPIXBUF
static void
pixbuf_free (guchar *pixels, gpointer data)
{
  g_free (pixels);
}
#endif

static void
read_pixels_to_file (char *filename_stem,
                     int x,
                     int y,
                     int width,
                     int height)
{
#ifdef USE_GDKPIXBUF
  GLubyte *data;
  GdkPixbuf *pixbuf;
  static int read_count = 0;

  data = g_malloc (4 * width * height);
  cogl_read_pixels (x, y, width, height,
                    COGL_READ_PIXELS_COLOR_BUFFER,
                    COGL_PIXEL_FORMAT_RGB_888,
                    data);
  pixbuf = gdk_pixbuf_new_from_data (data,
                                     GDK_COLORSPACE_RGB,
                                     FALSE, /* has alpha */
                                     8, /* bits per sample */
                                     width, /* width */
                                     height, /* height */
                                     width * 3, /* rowstride */
                                     pixbuf_free, /* callback to free data */
                                     NULL); /* callback data */
  if (pixbuf)
    {
      char *filename = g_strdup_printf ("%s-%05d.png",
                                        filename_stem,
                                        read_count);
      GError *error = NULL;

      if (!gdk_pixbuf_save (pixbuf, filename, "png", &error, NULL))
        {
          g_warning ("Failed to save pick buffer to file %s: %s",
                     filename, error->message);
          g_error_free (error);
        }

      g_free (filename);
      g_object_unref (pixbuf);
      read_count++;
    }
#else /* !USE_GDKPIXBUF */
  {
    static gboolean seen = FALSE;

    if (!seen)
      {
        g_warning ("dumping buffers to an image isn't supported on platforms "
                   "without gdk pixbuf support\n");
        seen = TRUE;
      }
  }
#endif /* USE_GDKPIXBUF */
}

ClutterActor *
_clutter_do_pick (ClutterStage   *stage,
		  gint            x,
		  gint            y,
		  ClutterPickMode mode)
{
  ClutterMainContext *context;
  guchar              pixel[4] = { 0xff, 0xff, 0xff, 0xff };
  CoglColor           stage_pick_id;
  guint32             id;
  GLboolean           dither_was_on;
  ClutterActor       *actor;
  CLUTTER_STATIC_COUNTER (do_pick_counter,
                          "_clutter_do_pick counter",
                          "Increments for each full pick run",
                          0 /* no application private data */);
  CLUTTER_STATIC_TIMER (pick_timer,
                        "Mainloop", /* parent */
                        "Picking",
                        "The time spent picking",
                        0 /* no application private data */);
  CLUTTER_STATIC_TIMER (pick_clear,
                        "Picking", /* parent */
                        "Stage clear (pick)",
                        "The time spent clearing stage for picking",
                        0 /* no application private data */);
  CLUTTER_STATIC_TIMER (pick_paint,
                        "Picking", /* parent */
                        "Painting actors (pick mode)",
                        "The time spent painting actors in pick mode",
                        0 /* no application private data */);
  CLUTTER_STATIC_TIMER (pick_read,
                        "Picking", /* parent */
                        "Read Pixels",
                        "The time spent issuing a read pixels",
                        0 /* no application private data */);

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  if (clutter_debug_flags & CLUTTER_DEBUG_NOP_PICKING)
    return CLUTTER_ACTOR (stage);

#ifdef CLUTTER_ENABLE_PROFILE
  if (clutter_profile_flags & CLUTTER_PROFILE_PICKING_ONLY)
    _clutter_profile_resume ();
#endif /* CLUTTER_ENABLE_PROFILE */

  CLUTTER_COUNTER_INC (_clutter_uprof_context, do_pick_counter);
  CLUTTER_TIMER_START (_clutter_uprof_context, pick_timer);

  context = _clutter_context_get_default ();

  _clutter_backend_ensure_context (context->backend, stage);

  /* needed for when a context switch happens */
  _clutter_stage_maybe_setup_viewport (stage);

  if (G_LIKELY (!(clutter_pick_debug_flags & CLUTTER_DEBUG_DUMP_PICK_BUFFERS)))
    cogl_clip_push_window_rectangle (x, y, 1, 1);

  cogl_disable_fog ();
  cogl_color_set_from_4ub (&stage_pick_id, 255, 255, 255, 255);
  CLUTTER_TIMER_START (_clutter_uprof_context, pick_clear);
  cogl_clear (&stage_pick_id,
	      COGL_BUFFER_BIT_COLOR |
	      COGL_BUFFER_BIT_DEPTH);
  CLUTTER_TIMER_STOP (_clutter_uprof_context, pick_clear);

  /* Disable dithering (if any) when doing the painting in pick mode */
  dither_was_on = glIsEnabled (GL_DITHER);
  if (dither_was_on)
    glDisable (GL_DITHER);

  /* Render the entire scence in pick mode - just single colored silhouette's
   * are drawn offscreen (as we never swap buffers)
  */
  CLUTTER_TIMER_START (_clutter_uprof_context, pick_paint);
  context->pick_mode = mode;
  clutter_actor_paint (CLUTTER_ACTOR (stage));
  context->pick_mode = CLUTTER_PICK_NONE;
  CLUTTER_TIMER_STOP (_clutter_uprof_context, pick_paint);

  if (G_LIKELY (!(clutter_pick_debug_flags & CLUTTER_DEBUG_DUMP_PICK_BUFFERS)))
    cogl_clip_pop ();

  /* Make sure Cogl flushes any batched geometry to the GPU driver */
  cogl_flush ();

  /* Read the color of the screen co-ords pixel. RGBA_8888_PRE is used
     even though we don't care about the alpha component because under
     GLES this is the only format that is guaranteed to work so Cogl
     will end up having to do a conversion if any other format is
     used. The format is requested as pre-multiplied because Cogl
     assumes that all pixels in the framebuffer are premultiplied so
     it avoids a conversion. */
  CLUTTER_TIMER_START (_clutter_uprof_context, pick_read);
  cogl_read_pixels (x, y, 1, 1,
                    COGL_READ_PIXELS_COLOR_BUFFER,
                    COGL_PIXEL_FORMAT_RGBA_8888_PRE,
                    pixel);
  CLUTTER_TIMER_STOP (_clutter_uprof_context, pick_read);

  if (G_UNLIKELY (clutter_pick_debug_flags & CLUTTER_DEBUG_DUMP_PICK_BUFFERS))
    {
      read_pixels_to_file ("pick-buffer", 0, 0,
                           clutter_actor_get_width (CLUTTER_ACTOR (stage)),
                           clutter_actor_get_height (CLUTTER_ACTOR (stage)));
    }

  /* Restore whether GL_DITHER was enabled */
  if (dither_was_on)
    glEnable (GL_DITHER);

  if (pixel[0] == 0xff && pixel[1] == 0xff && pixel[2] == 0xff)
    {
      actor = CLUTTER_ACTOR (stage);
      goto result;
    }

  id = _clutter_pixel_to_id (pixel);
  actor = clutter_get_actor_by_gid (id);

result:

  CLUTTER_TIMER_STOP (_clutter_uprof_context, pick_timer);

#ifdef CLUTTER_ENABLE_PROFILE
  if (clutter_profile_flags & CLUTTER_PROFILE_PICKING_ONLY)
    _clutter_profile_suspend ();
#endif

  return actor;
}

CoglPangoFontMap *
_clutter_context_get_pango_fontmap (ClutterMainContext *self)
{
  CoglPangoFontMap *font_map;
  gdouble resolution;
  gboolean use_mipmapping;

  if (G_LIKELY (self->font_map != NULL))
    return self->font_map;

  font_map = COGL_PANGO_FONT_MAP (cogl_pango_font_map_new ());

  resolution = clutter_backend_get_resolution (self->backend);
  cogl_pango_font_map_set_resolution (font_map, resolution);

  use_mipmapping = !clutter_disable_mipmap_text;
  cogl_pango_font_map_set_use_mipmapping (font_map, use_mipmapping);

  self->font_map = font_map;

  return self->font_map;
}

static ClutterTextDirection
clutter_get_text_direction (void)
{
  PangoDirection dir = PANGO_DIRECTION_LTR;
  const gchar *direction;

  direction = g_getenv ("CLUTTER_TEXT_DIRECTION");
  if (direction && *direction != '\0')
    {
      if (strcmp (direction, "rtl") == 0)
        dir = CLUTTER_TEXT_DIRECTION_RTL;
      else if (strcmp (direction, "ltr") == 0)
        dir = CLUTTER_TEXT_DIRECTION_LTR;
    }
  else
    {
      /* Translate to default:RTL if you want your widgets
       * to be RTL, otherwise translate to default:LTR.
       *
       * Do *not* translate it to "predefinito:LTR": if it
       * it isn't default:LTR or default:RTL it will not work
       */
      char *e = _("default:LTR");

      if (strcmp (e, "default:RTL") == 0)
        dir = CLUTTER_TEXT_DIRECTION_RTL;
      else if (strcmp (e, "default:LTR") == 0)
        dir = CLUTTER_TEXT_DIRECTION_LTR;
      else
        g_warning ("Whoever translated default:LTR did so wrongly.");
    }

  return dir;
}

static void
update_pango_context (ClutterBackend *backend,
                      PangoContext   *context)
{
  PangoFontDescription *font_desc;
  const cairo_font_options_t *font_options;
  const gchar *font_name;
  PangoDirection pango_dir;
  gdouble resolution;

  /* update the text direction */
  if (clutter_text_direction == CLUTTER_TEXT_DIRECTION_RTL)
    pango_dir = PANGO_DIRECTION_RTL;
  else
    pango_dir = PANGO_DIRECTION_LTR;

  pango_context_set_base_dir (context, pango_dir);

  /* get the configuration for the PangoContext from the backend */
  font_name = clutter_backend_get_font_name (backend);
  font_options = clutter_backend_get_font_options (backend);
  resolution = clutter_backend_get_resolution (backend);

  font_desc = pango_font_description_from_string (font_name);

  if (resolution < 0)
    resolution = 96.0; /* fall back */

  pango_context_set_font_description (context, font_desc);
  pango_cairo_context_set_font_options (context, font_options);
  pango_cairo_context_set_resolution (context, resolution);

  pango_font_description_free (font_desc);
}

PangoContext *
_clutter_context_get_pango_context (ClutterMainContext *self)
{
  if (G_UNLIKELY (self->pango_context == NULL))
    {
      PangoContext *context;

      context = _clutter_context_create_pango_context (self);
      self->pango_context = context;

      g_signal_connect (self->backend, "resolution-changed",
                        G_CALLBACK (update_pango_context),
                        self->pango_context);
      g_signal_connect (self->backend, "font-changed",
                        G_CALLBACK (update_pango_context),
                        self->pango_context);
    }
  else
    update_pango_context (self->backend, self->pango_context);

  return self->pango_context;
}

PangoContext *
_clutter_context_create_pango_context (ClutterMainContext *self)
{
  CoglPangoFontMap *font_map;
  PangoContext *context;

  font_map = _clutter_context_get_pango_fontmap (self);

  context = cogl_pango_font_map_create_context (font_map);
  update_pango_context (self->backend, context);
  pango_context_set_language (context, pango_language_get_default ());

  return context;
}

/**
 * clutter_main_quit:
 *
 * Terminates the Clutter mainloop.
 */
void
clutter_main_quit (void)
{
  g_return_if_fail (main_loops != NULL);

  g_main_loop_quit (main_loops->data);
}

/**
 * clutter_main_level:
 *
 * Retrieves the depth of the Clutter mainloop.
 *
 * Return value: The level of the mainloop.
 */
gint
clutter_main_level (void)
{
  return clutter_main_loop_level;
}

#ifdef CLUTTER_ENABLE_PROFILE
static gint (*prev_poll) (GPollFD *ufds, guint nfsd, gint timeout_) = NULL;

static gint
timed_poll (GPollFD *ufds,
            guint nfsd,
            gint timeout_)
{
  gint ret;
  CLUTTER_STATIC_TIMER (poll_timer,
                        "Mainloop", /* parent */
                        "poll (idle)",
                        "The time spent idle in poll()",
                        0 /* no application private data */);

  CLUTTER_TIMER_START (_clutter_uprof_context, poll_timer);
  ret = prev_poll (ufds, nfsd, timeout_);
  CLUTTER_TIMER_STOP (_clutter_uprof_context, poll_timer);
  return ret;
}
#endif

/**
 * clutter_main:
 *
 * Starts the Clutter mainloop.
 */
void
clutter_main (void)
{
  GMainLoop *loop;
  CLUTTER_STATIC_TIMER (mainloop_timer,
                        NULL, /* no parent */
                        "Mainloop",
                        "The time spent in the clutter mainloop",
                        0 /* no application private data */);

  if (clutter_main_loop_level == 0)
    CLUTTER_TIMER_START (_clutter_uprof_context, mainloop_timer);

  /* Make sure there is a context */
  CLUTTER_CONTEXT ();

  if (!clutter_is_initialized)
    {
      g_warning ("Called clutter_main() but Clutter wasn't initialised.  "
		 "You must call clutter_init() first.");
      return;
    }

  CLUTTER_MARK ();

  clutter_main_loop_level++;

#ifdef CLUTTER_ENABLE_PROFILE
  if (!prev_poll)
    {
      prev_poll = g_main_context_get_poll_func (NULL);
      g_main_context_set_poll_func (NULL, timed_poll);
    }
#endif

  loop = g_main_loop_new (NULL, TRUE);
  main_loops = g_slist_prepend (main_loops, loop);

#ifdef HAVE_CLUTTER_FRUITY
  /* clutter fruity creates an application that forwards events and manually
   * spins the mainloop
   */
  clutter_fruity_main ();
#else
  if (g_main_loop_is_running (main_loops->data))
    {
      clutter_threads_leave ();
      g_main_loop_run (loop);
      clutter_threads_enter ();
    }
#endif

  main_loops = g_slist_remove (main_loops, loop);

  g_main_loop_unref (loop);

  clutter_main_loop_level--;

  CLUTTER_MARK ();

  if (clutter_main_loop_level == 0)
    CLUTTER_TIMER_STOP (_clutter_uprof_context, mainloop_timer);
}

static void
clutter_threads_impl_lock (void)
{
  if (G_LIKELY (clutter_threads_mutex != NULL))
    g_mutex_lock (clutter_threads_mutex);
}

static void
clutter_threads_impl_unlock (void)
{
  if (G_LIKELY (clutter_threads_mutex != NULL))
    g_mutex_unlock (clutter_threads_mutex);
}

/**
 * clutter_threads_init:
 *
 * Initialises the Clutter threading mechanism, so that Clutter API can be
 * called by multiple threads, using clutter_threads_enter() and
 * clutter_threads_leave() to mark the critical sections.
 *
 * You must call g_thread_init() before this function.
 *
 * This function must be called before clutter_init().
 *
 * It is safe to call this function multiple times.
 *
 * Since: 0.4
 */
void
clutter_threads_init (void)
{
  if (!g_thread_supported ())
    g_error ("g_thread_init() must be called before clutter_threads_init()");

  if (clutter_threads_mutex != NULL)
    return;

  clutter_threads_mutex = g_mutex_new ();

  if (!clutter_threads_lock)
    clutter_threads_lock = clutter_threads_impl_lock;

  if (!clutter_threads_unlock)
    clutter_threads_unlock = clutter_threads_impl_unlock;
}

/**
 * clutter_threads_set_lock_functions:
 * @enter_fn: function called when aquiring the Clutter main lock
 * @leave_fn: function called when releasing the Clutter main lock
 *
 * Allows the application to replace the standard method that
 * Clutter uses to protect its data structures. Normally, Clutter
 * creates a single #GMutex that is locked by clutter_threads_enter(),
 * and released by clutter_threads_leave(); using this function an
 * application provides, instead, a function @enter_fn that is
 * called by clutter_threads_enter() and a function @leave_fn that is
 * called by clutter_threads_leave().
 *
 * The functions must provide at least same locking functionality
 * as the default implementation, but can also do extra application
 * specific processing.
 *
 * As an example, consider an application that has its own recursive
 * lock that when held, holds the Clutter lock as well. When Clutter
 * unlocks the Clutter lock when entering a recursive main loop, the
 * application must temporarily release its lock as well.
 *
 * Most threaded Clutter apps won't need to use this method.
 *
 * This method must be called before clutter_threads_init(), and cannot
 * be called multiple times.
 *
 * Since: 0.4
 */
void
clutter_threads_set_lock_functions (GCallback enter_fn,
                                    GCallback leave_fn)
{
  g_return_if_fail (clutter_threads_lock == NULL &&
                    clutter_threads_unlock == NULL);

  clutter_threads_lock = enter_fn;
  clutter_threads_unlock = leave_fn;
}

typedef struct
{
  GSourceFunc func;
  gpointer data;
  GDestroyNotify notify;
} ClutterThreadsDispatch;

static gboolean
clutter_threads_dispatch (gpointer data)
{
  ClutterThreadsDispatch *dispatch = data;
  gboolean ret = FALSE;

  clutter_threads_enter ();

  if (!g_source_is_destroyed (g_main_current_source ()))
    ret = dispatch->func (dispatch->data);

  clutter_threads_leave ();

  return ret;
}

static void
clutter_threads_dispatch_free (gpointer data)
{
  ClutterThreadsDispatch *dispatch = data;

  /* XXX - we cannot hold the thread lock here because the main loop
   * might destroy a source while still in the dispatcher function; so
   * knowing whether the lock is being held or not is not known a priori.
   *
   * see bug: http://bugzilla.gnome.org/show_bug.cgi?id=459555
   */
  if (dispatch->notify)
    dispatch->notify (dispatch->data);

  g_slice_free (ClutterThreadsDispatch, dispatch);
}

/**
 * clutter_threads_add_idle_full:
 * @priority: the priority of the timeout source. Typically this will be in the
 *    range between #G_PRIORITY_DEFAULT_IDLE and #G_PRIORITY_HIGH_IDLE
 * @func: function to call
 * @data: data to pass to the function
 * @notify: functio to call when the idle source is removed
 *
 * Adds a function to be called whenever there are no higher priority
 * events pending. If the function returns %FALSE it is automatically
 * removed from the list of event sources and will not be called again.
 *
 * This function can be considered a thread-safe variant of g_idle_add_full():
 * it will call @function while holding the Clutter lock. It is logically
 * equivalent to the following implementation:
 *
 * |[
 * static gboolean
 * idle_safe_callback (gpointer data)
 * {
 *    SafeClosure *closure = data;
 *    gboolean res = FALSE;
 *
 *    /&ast; mark the critical section &ast;/
 *
 *    clutter_threads_enter();
 *
 *    /&ast; the callback does not need to acquire the Clutter
 *     &ast; lock itself, as it is held by the this proxy handler
 *     &ast;/
 *    res = closure->callback (closure->data);
 *
 *    clutter_threads_leave();
 *
 *    return res;
 * }
 * static gulong
 * add_safe_idle (GSourceFunc callback,
 *                gpointer    data)
 * {
 *   SafeClosure *closure = g_new0 (SafeClosure, 1);
 *
 *   closure-&gt;callback = callback;
 *   closure-&gt;data = data;
 *
 *   return g_add_idle_full (G_PRIORITY_DEFAULT_IDLE,
 *                           idle_safe_callback,
 *                           closure,
 *                           g_free)
 * }
 *]|
 *
 * This function should be used by threaded applications to make sure
 * that @func is emitted under the Clutter threads lock and invoked
 * from the same thread that started the Clutter main loop. For instance,
 * it can be used to update the UI using the results from a worker
 * thread:
 *
 * |[
 * static gboolean
 * update_ui (gpointer data)
 * {
 *   SomeClosure *closure = data;
 *
 *   /&ast; it is safe to call Clutter API from this function because
 *    &ast; it is invoked from the same thread that started the main
 *    &ast; loop and under the Clutter thread lock
 *    &ast;/
 *   clutter_label_set_text (CLUTTER_LABEL (closure-&gt;label),
 *                           closure-&gt;text);
 *
 *   g_object_unref (closure-&gt;label);
 *   g_free (closure);
 *
 *   return FALSE;
 * }
 *
 *   /&ast; within another thread &ast;/
 *   closure = g_new0 (SomeClosure, 1);
 *   /&ast; always take a reference on GObject instances &ast;/
 *   closure-&gt;label = g_object_ref (my_application-&gt;label);
 *   closure-&gt;text = g_strdup (processed_text_to_update_the_label);
 *
 *   clutter_threads_add_idle_full (G_PRIORITY_HIGH_IDLE,
 *                                  update_ui,
 *                                  closure,
 *                                  NULL);
 * ]|
 *
 * Return value: the ID (greater than 0) of the event source.
 *
 * Since: 0.4
 */
guint
clutter_threads_add_idle_full (gint           priority,
                               GSourceFunc    func,
                               gpointer       data,
                               GDestroyNotify notify)
{
  ClutterThreadsDispatch *dispatch;

  g_return_val_if_fail (func != NULL, 0);

  dispatch = g_slice_new (ClutterThreadsDispatch);
  dispatch->func = func;
  dispatch->data = data;
  dispatch->notify = notify;

  return g_idle_add_full (priority,
                          clutter_threads_dispatch, dispatch,
                          clutter_threads_dispatch_free);
}

/**
 * clutter_threads_add_idle:
 * @func: function to call
 * @data: data to pass to the function
 *
 * Simple wrapper around clutter_threads_add_idle_full() using the
 * default priority.
 *
 * Return value: the ID (greater than 0) of the event source.
 *
 * Since: 0.4
 */
guint
clutter_threads_add_idle (GSourceFunc func,
                          gpointer    data)
{
  g_return_val_if_fail (func != NULL, 0);

  return clutter_threads_add_idle_full (G_PRIORITY_DEFAULT_IDLE,
                                        func, data,
                                        NULL);
}

/**
 * clutter_threads_add_timeout_full:
 * @priority: the priority of the timeout source. Typically this will be in the
 *            range between #G_PRIORITY_DEFAULT and #G_PRIORITY_HIGH.
 * @interval: the time between calls to the function, in milliseconds
 * @func: function to call
 * @data: data to pass to the function
 * @notify: function to call when the timeout source is removed
 *
 * Sets a function to be called at regular intervals holding the Clutter
 * threads lock, with the given priority. The function is called repeatedly
 * until it returns %FALSE, at which point the timeout is automatically
 * removed and the function will not be called again. The @notify function
 * is called when the timeout is removed.
 *
 * The first call to the function will be at the end of the first @interval.
 *
 * It is important to note that, due to how the Clutter main loop is
 * implemented, the timing will not be accurate and it will not try to
 * "keep up" with the interval. A more reliable source is available
 * using clutter_threads_add_frame_source_full(), which is also internally
 * used by #ClutterTimeline.
 *
 * See also clutter_threads_add_idle_full().
 *
 * Return value: the ID (greater than 0) of the event source.
 *
 * Since: 0.4
 */
guint
clutter_threads_add_timeout_full (gint           priority,
                                  guint          interval,
                                  GSourceFunc    func,
                                  gpointer       data,
                                  GDestroyNotify notify)
{
  ClutterThreadsDispatch *dispatch;

  g_return_val_if_fail (func != NULL, 0);

  dispatch = g_slice_new (ClutterThreadsDispatch);
  dispatch->func = func;
  dispatch->data = data;
  dispatch->notify = notify;

  return g_timeout_add_full (priority,
                             interval,
                             clutter_threads_dispatch, dispatch,
                             clutter_threads_dispatch_free);
}

/**
 * clutter_threads_add_timeout:
 * @interval: the time between calls to the function, in milliseconds
 * @func: function to call
 * @data: data to pass to the function
 *
 * Simple wrapper around clutter_threads_add_timeout_full().
 *
 * Return value: the ID (greater than 0) of the event source.
 *
 * Since: 0.4
 */
guint
clutter_threads_add_timeout (guint       interval,
                             GSourceFunc func,
                             gpointer    data)
{
  g_return_val_if_fail (func != NULL, 0);

  return clutter_threads_add_timeout_full (G_PRIORITY_DEFAULT,
                                           interval,
                                           func, data,
                                           NULL);
}

/**
 * clutter_threads_add_frame_source_full:
 * @priority: the priority of the frame source. Typically this will be in the
 *            range between #G_PRIORITY_DEFAULT and #G_PRIORITY_HIGH.
 * @fps: the number of times per second to call the function
 * @func: function to call
 * @data: data to pass to the function
 * @notify: function to call when the timeout source is removed
 *
 * Sets a function to be called at regular intervals holding the Clutter
 * threads lock, with the given priority. The function is called repeatedly
 * until it returns %FALSE, at which point the timeout is automatically
 * removed and the function will not be called again. The @notify function
 * is called when the timeout is removed.
 *
 * This function is similar to clutter_threads_add_timeout_full()
 * except that it will try to compensate for delays. For example, if
 * @func takes half the interval time to execute then the function
 * will be called again half the interval time after it finished. In
 * contrast clutter_threads_add_timeout_full() would not fire until a
 * full interval after the function completes so the delay between
 * calls would be @interval * 1.5. This function does not however try
 * to invoke the function multiple times to catch up missing frames if
 * @func takes more than @interval ms to execute.
 *
 * See also clutter_threads_add_idle_full().
 *
 * Return value: the ID (greater than 0) of the event source.
 *
 * Since: 0.8
 */
guint
clutter_threads_add_frame_source_full (gint           priority,
				       guint          fps,
				       GSourceFunc    func,
				       gpointer       data,
				       GDestroyNotify notify)
{
  ClutterThreadsDispatch *dispatch;

  g_return_val_if_fail (func != NULL, 0);

  dispatch = g_slice_new (ClutterThreadsDispatch);
  dispatch->func = func;
  dispatch->data = data;
  dispatch->notify = notify;

  return clutter_frame_source_add_full (priority,
					fps,
					clutter_threads_dispatch, dispatch,
					clutter_threads_dispatch_free);
}

/**
 * clutter_threads_add_frame_source:
 * @fps: the number of times per second to call the function
 * @func: function to call
 * @data: data to pass to the function
 *
 * Simple wrapper around clutter_threads_add_frame_source_full().
 *
 * Return value: the ID (greater than 0) of the event source.
 *
 * Since: 0.8
 */
guint
clutter_threads_add_frame_source (guint       fps,
				  GSourceFunc func,
				  gpointer    data)
{
  g_return_val_if_fail (func != NULL, 0);

  return clutter_threads_add_frame_source_full (G_PRIORITY_DEFAULT,
						fps,
						func, data,
						NULL);
}

/**
 * clutter_threads_enter:
 *
 * Locks the Clutter thread lock.
 *
 * Since: 0.4
 */
void
clutter_threads_enter (void)
{
  if (clutter_threads_lock)
    (* clutter_threads_lock) ();
}

/**
 * clutter_threads_leave:
 *
 * Unlocks the Clutter thread lock.
 *
 * Since: 0.4
 */
void
clutter_threads_leave (void)
{
  if (clutter_threads_unlock)
    (* clutter_threads_unlock) ();
}


/**
 * clutter_get_debug_enabled:
 *
 * Check if clutter has debugging turned on.
 *
 * Return value: TRUE if debugging is turned on, FALSE otherwise.
 */
gboolean
clutter_get_debug_enabled (void)
{
#ifdef CLUTTER_ENABLE_DEBUG
  return clutter_debug_flags != 0;
#else
  return FALSE;
#endif
}

gboolean
_clutter_context_is_initialized (void)
{
  if (ClutterCntx == NULL)
    return FALSE;

  return ClutterCntx->is_initialized;
}

ClutterMainContext *
_clutter_context_get_default (void)
{
  if (G_UNLIKELY (ClutterCntx == NULL))
    {
      ClutterMainContext *ctx;

      ClutterCntx = ctx = g_new0 (ClutterMainContext, 1);

      /* create the default backend */
      ctx->backend = g_object_new (_clutter_backend_impl_get_type (), NULL);

      ctx->is_initialized = FALSE;
      ctx->motion_events_per_actor = TRUE;

#ifdef CLUTTER_ENABLE_DEBUG
      ctx->timer = g_timer_new ();
      g_timer_start (ctx->timer);
#endif
    }

  return ClutterCntx;
}

/**
 * clutter_get_timestamp:
 *
 * Returns the approximate number of microseconds passed since clutter was
 * intialised.
 *
 * Return value: Number of microseconds since clutter_init() was called.
 */
gulong
clutter_get_timestamp (void)
{
#ifdef CLUTTER_ENABLE_DEBUG
  ClutterMainContext *ctx;
  gdouble seconds;

  ctx = _clutter_context_get_default ();

  /* FIXME: may need a custom timer for embedded setups */
  seconds = g_timer_elapsed (ctx->timer, NULL);

  return (gulong)(seconds / 1.0e-6);
#else
  return 0;
#endif
}

static gboolean
clutter_arg_direction_cb (const char *key,
                          const char *value,
                          gpointer    user_data)
{
  clutter_text_direction =
    (strcmp (value, "rtl") == 0) ? CLUTTER_TEXT_DIRECTION_RTL
                                 : CLUTTER_TEXT_DIRECTION_LTR;

  return TRUE;
}

#ifdef CLUTTER_ENABLE_DEBUG
static gboolean
clutter_arg_debug_cb (const char *key,
                      const char *value,
                      gpointer    user_data)
{
  clutter_debug_flags |=
    g_parse_debug_string (value,
                          clutter_debug_keys,
                          G_N_ELEMENTS (clutter_debug_keys));
  return TRUE;
}

static gboolean
clutter_arg_no_debug_cb (const char *key,
                         const char *value,
                         gpointer    user_data)
{
  clutter_debug_flags &=
    ~g_parse_debug_string (value,
                           clutter_debug_keys,
                           G_N_ELEMENTS (clutter_debug_keys));
  return TRUE;
}
#endif /* CLUTTER_ENABLE_DEBUG */

#ifdef CLUTTER_ENABLE_PROFILE
static gboolean
clutter_arg_profile_cb (const char *key,
                        const char *value,
                        gpointer    user_data)
{
  clutter_profile_flags |=
    g_parse_debug_string (value,
                          clutter_profile_keys,
                          G_N_ELEMENTS (clutter_profile_keys));
  return TRUE;
}

static gboolean
clutter_arg_no_profile_cb (const char *key,
                           const char *value,
                           gpointer    user_data)
{
  clutter_profile_flags &=
    ~g_parse_debug_string (value,
                           clutter_profile_keys,
                           G_N_ELEMENTS (clutter_profile_keys));
  return TRUE;
}
#endif /* CLUTTER_ENABLE_PROFILE */

GQuark
clutter_init_error_quark (void)
{
  return g_quark_from_static_string ("clutter-init-error-quark");
}

static ClutterInitError
clutter_init_real (GError **error)
{
  ClutterMainContext *ctx;
  ClutterBackend *backend;

  /* Note, creates backend if not already existing, though parse args will
   * have likely created it
   */
  ctx = _clutter_context_get_default ();
  backend = ctx->backend;

  if (!ctx->options_parsed)
    {
      if (error)
        g_set_error (error, CLUTTER_INIT_ERROR,
                     CLUTTER_INIT_ERROR_INTERNAL,
                     "When using clutter_get_option_group_without_init() "
		     "you must parse options before calling clutter_init()");
      else
        g_critical ("When using clutter_get_option_group_without_init() "
		    "you must parse options before calling clutter_init()");

      return CLUTTER_INIT_ERROR_INTERNAL;
    }

  /*
   * Call backend post parse hooks.
   */
  if (!_clutter_backend_post_parse (backend, error))
    return CLUTTER_INIT_ERROR_BACKEND;

  /* this will take care of initializing Cogl's state and
   * query the GL machinery for features
   */
  if (!_clutter_feature_init (error))
    return CLUTTER_INIT_ERROR_BACKEND;

#ifdef CLUTTER_ENABLE_PROFILE
    {
      UProfContext *cogl_context;
      cogl_context = uprof_find_context ("Cogl");
      if (cogl_context)
        uprof_context_link (_clutter_uprof_context, cogl_context);
    }

  if (clutter_profile_flags & CLUTTER_PROFILE_PICKING_ONLY)
    _clutter_profile_suspend ();
#endif

  clutter_text_direction = clutter_get_text_direction ();

  /* Figure out framebuffer masks used for pick */
  cogl_get_bitmasks (&ctx->fb_r_mask, &ctx->fb_g_mask, &ctx->fb_b_mask, NULL);

  ctx->fb_r_mask_used = ctx->fb_r_mask;
  ctx->fb_g_mask_used = ctx->fb_g_mask;
  ctx->fb_b_mask_used = ctx->fb_b_mask;

  /* XXX - describe what "fuzzy picking" is */
  if (clutter_use_fuzzy_picking)
    {
      ctx->fb_r_mask_used--;
      ctx->fb_g_mask_used--;
      ctx->fb_b_mask_used--;
    }

  /* Initiate event collection */
  _clutter_backend_init_events (ctx->backend);

  clutter_is_initialized = TRUE;
  ctx->is_initialized = TRUE;

  /* Initialize a11y */
  if (clutter_enable_accessibility)
    cally_accessibility_init ();

  return CLUTTER_INIT_SUCCESS;
}

static GOptionEntry clutter_args[] = {
  { "clutter-show-fps", 0, 0, G_OPTION_ARG_NONE, &clutter_show_fps,
    N_("Show frames per second"), NULL },
  { "clutter-default-fps", 0, 0, G_OPTION_ARG_INT, &clutter_default_fps,
    N_("Default frame rate"), "FPS" },
  { "g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &clutter_fatal_warnings,
    N_("Make all warnings fatal"), NULL },
  { "clutter-text-direction", 0, 0, G_OPTION_ARG_CALLBACK,
    clutter_arg_direction_cb,
    N_("Direction for the text"), "DIRECTION" },
  { "clutter-disable-mipmapped-text", 0, 0, G_OPTION_ARG_NONE,
    &clutter_disable_mipmap_text,
    N_("Disable mipmapping on text"), NULL },
  { "clutter-use-fuzzy-picking", 0, 0, G_OPTION_ARG_NONE,
    &clutter_use_fuzzy_picking,
    N_("Use 'fuzzy' picking"), NULL },
#ifdef CLUTTER_ENABLE_DEBUG
  { "clutter-debug", 0, 0, G_OPTION_ARG_CALLBACK, clutter_arg_debug_cb,
    N_("Clutter debugging flags to set"), "FLAGS" },
  { "clutter-no-debug", 0, 0, G_OPTION_ARG_CALLBACK, clutter_arg_no_debug_cb,
    N_("Clutter debugging flags to unset"), "FLAGS" },
#endif /* CLUTTER_ENABLE_DEBUG */
#ifdef CLUTTER_ENABLE_PROFILE
  { "clutter-profile", 0, 0, G_OPTION_ARG_CALLBACK, clutter_arg_profile_cb,
    N_("Clutter profiling flags to set"), "FLAGS" },
  { "clutter-no-profile", 0, 0, G_OPTION_ARG_CALLBACK, clutter_arg_no_profile_cb,
    N_("Clutter profiling flags to unset"), "FLAGS" },
#endif /* CLUTTER_ENABLE_PROFILE */
  { "clutter-enable-accessibility", 0, 0, G_OPTION_ARG_NONE, &clutter_enable_accessibility,
    N_("Enable accessibility"), NULL },
  { NULL, },
};

/* pre_parse_hook: initialise variables depending on environment
 * variables; these variables might be overridden by the command
 * line arguments that are going to be parsed after.
 */
static gboolean
pre_parse_hook (GOptionContext  *context,
                GOptionGroup    *group,
                gpointer         data,
                GError         **error)
{
  ClutterMainContext *clutter_context;
  ClutterBackend *backend;
  const char *env_string;

  if (clutter_is_initialized)
    return TRUE;

  if (setlocale (LC_ALL, "") == NULL)
    g_warning ("Locale not supported by C library.\n"
               "Using the fallback 'C' locale.");

  clutter_context = _clutter_context_get_default ();

  clutter_context->id_pool = clutter_id_pool_new (256);

  backend = clutter_context->backend;
  g_assert (CLUTTER_IS_BACKEND (backend));

#ifdef CLUTTER_ENABLE_DEBUG
  env_string = g_getenv ("CLUTTER_DEBUG");
  if (env_string != NULL)
    {
      clutter_debug_flags =
        g_parse_debug_string (env_string,
                              clutter_debug_keys,
                              G_N_ELEMENTS (clutter_debug_keys));
      env_string = NULL;
    }
#endif /* CLUTTER_ENABLE_DEBUG */

#ifdef CLUTTER_ENABLE_PROFILE
  env_string = g_getenv ("CLUTTER_PROFILE");
  if (env_string != NULL)
    {
      clutter_profile_flags =
        g_parse_debug_string (env_string,
                              clutter_profile_keys,
                              G_N_ELEMENTS (clutter_profile_keys));
      env_string = NULL;
    }
#endif /* CLUTTER_ENABLE_PROFILE */

  env_string = g_getenv ("CLUTTER_PICK");
  if (env_string != NULL)
    {
      clutter_pick_debug_flags =
        g_parse_debug_string (env_string,
                              clutter_pick_debug_keys,
                              G_N_ELEMENTS (clutter_pick_debug_keys));
      env_string = NULL;
    }

  env_string = g_getenv ("CLUTTER_PAINT");
  if (env_string != NULL)
    {
      clutter_paint_debug_flags =
        g_parse_debug_string (env_string,
                              clutter_paint_debug_keys,
                              G_N_ELEMENTS (clutter_paint_debug_keys));
      env_string = NULL;
    }

  env_string = g_getenv ("CLUTTER_SHOW_FPS");
  if (env_string)
    clutter_show_fps = TRUE;

  env_string = g_getenv ("CLUTTER_DEFAULT_FPS");
  if (env_string)
    {
      gint default_fps = g_ascii_strtoll (env_string, NULL, 10);

      clutter_default_fps = CLAMP (default_fps, 1, 1000);
    }

  env_string = g_getenv ("CLUTTER_DISABLE_MIPMAPPED_TEXT");
  if (env_string)
    clutter_disable_mipmap_text = TRUE;

#ifdef HAVE_CLUTTER_FRUITY
  /* we always enable fuzzy picking in the "fruity" backend */
  clutter_use_fuzzy_picking = TRUE;
#else
  env_string = g_getenv ("CLUTTER_FUZZY_PICK");
  if (env_string)
    clutter_use_fuzzy_picking = TRUE;
#endif /* HAVE_CLUTTER_FRUITY */

  return _clutter_backend_pre_parse (backend, error);
}

/* post_parse_hook: initialise the context and data structures
 * and opens the X display
 */
static gboolean
post_parse_hook (GOptionContext  *context,
                 GOptionGroup    *group,
                 gpointer         data,
                 GError         **error)
{
  ClutterMainContext *clutter_context;
  ClutterBackend *backend;

  if (clutter_is_initialized)
    return TRUE;

  clutter_context = _clutter_context_get_default ();
  backend = clutter_context->backend;
  g_assert (CLUTTER_IS_BACKEND (backend));

  if (clutter_fatal_warnings)
    {
      GLogLevelFlags fatal_mask;

      fatal_mask = g_log_set_always_fatal (G_LOG_FATAL_MASK);
      fatal_mask |= G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL;
      g_log_set_always_fatal (fatal_mask);
    }

  clutter_context->frame_rate = clutter_default_fps;
  clutter_context->options_parsed = TRUE;

  /*
   * If not asked to defer display setup, call clutter_init_real(),
   * which in turn calls the backend post parse hooks.
   */
  if (!clutter_context->defer_display_setup)
    return clutter_init_real (error);

  return TRUE;
}

/**
 * clutter_get_option_group:
 *
 * Returns a #GOptionGroup for the command line arguments recognized
 * by Clutter. You should add this group to your #GOptionContext with
 * g_option_context_add_group(), if you are using g_option_context_parse()
 * to parse your commandline arguments.
 *
 * Calling g_option_context_parse() with Clutter's #GOptionGroup will result
 * in Clutter's initialization. That is, the following code:
 *
 * |[
 *   g_option_context_set_main_group (context, clutter_get_option_group ());
 *   res = g_option_context_parse (context, &amp;argc, &amp;argc, NULL);
 * ]|
 *
 * is functionally equivalent to:
 *
 * |[
 *   clutter_init (&amp;argc, &amp;argv);
 * ]|
 *
 * After g_option_context_parse() on a #GOptionContext containing the
 * Clutter #GOptionGroup has returned %TRUE, Clutter is guaranteed to be
 * initialized.
 *
 * Return value: (transfer full): a #GOptionGroup for the commandline arguments
 *   recognized by Clutter
 *
 * Since: 0.2
 */
GOptionGroup *
clutter_get_option_group (void)
{
  ClutterMainContext *context;
  GOptionGroup *group;

  clutter_base_init ();

  context = _clutter_context_get_default ();

  group = g_option_group_new ("clutter",
                              _("Clutter Options"),
                              _("Show Clutter Options"),
                              NULL,
                              NULL);

  g_option_group_set_parse_hooks (group, pre_parse_hook, post_parse_hook);
  g_option_group_add_entries (group, clutter_args);
  g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);

  /* add backend-specific options */
  _clutter_backend_add_options (context->backend, group);

  return group;
}

/**
 * clutter_get_option_group_without_init:
 *
 * Returns a #GOptionGroup for the command line arguments recognized
 * by Clutter. You should add this group to your #GOptionContext with
 * g_option_context_add_group(), if you are using g_option_context_parse()
 * to parse your commandline arguments. Unlike clutter_get_option_group(),
 * calling g_option_context_parse() with the #GOptionGroup returned by this
 * function requires a subsequent explicit call to clutter_init(); use this
 * function when needing to set foreign display connection with
 * clutter_x11_set_display(), or with gtk_clutter_init().
 *
 * Return value: (transfer full): a #GOptionGroup for the commandline arguments
 *   recognized by Clutter
 *
 * Since: 0.8.2
 */
GOptionGroup *
clutter_get_option_group_without_init (void)
{
  ClutterMainContext *context;
  GOptionGroup *group;

  clutter_base_init ();

  context = _clutter_context_get_default ();
  context->defer_display_setup = TRUE;

  group = clutter_get_option_group ();

  return group;
}

/* Note that the gobject-introspection annotations for the argc/argv
 * parameters do not produce the right result; however, they do
 * allow the common case of argc=NULL, argv=NULL to work.
 */

/**
 * clutter_init_with_args:
 * @argc: (inout): a pointer to the number of command line arguments
 * @argv: (array length=argc) (inout) (allow-none): a pointer to the array
 *   of command line arguments
 * @parameter_string: (allow-none): a string which is displayed in the
 *   first line of <option>--help</option> output, after
 *   <literal><replaceable>programname</replaceable> [OPTION...]</literal>
 * @entries: (allow-none): a %NULL terminated array of #GOptionEntry<!-- -->s
 *   describing the options of your program
 * @translation_domain: (allow-none): a translation domain to use for
 *   translating the <option>--help</option> output for the options in
 *   @entries with gettext(), or %NULL
 * @error: (allow-none): a return location for a #GError
 *
 * This function does the same work as clutter_init(). Additionally,
 * it allows you to add your own command line options, and it
 * automatically generates nicely formatted <option>--help</option>
 * output. Note that your program will be terminated after writing
 * out the help output. Also note that, in case of error, the
 * error message will be placed inside @error instead of being
 * printed on the display.
 *
 * Return value: %CLUTTER_INIT_SUCCESS if Clutter has been successfully
 *   initialised, or other values or #ClutterInitError in case of
 *   error.
 *
 * Since: 0.2
 */
ClutterInitError
clutter_init_with_args (int            *argc,
                        char         ***argv,
                        const char     *parameter_string,
                        GOptionEntry   *entries,
                        const char     *translation_domain,
                        GError        **error)
{
  GOptionContext *context;
  GOptionGroup *group;
  gboolean res;
  ClutterMainContext *ctx;

  if (clutter_is_initialized)
    return CLUTTER_INIT_SUCCESS;

  clutter_base_init ();

  ctx = _clutter_context_get_default ();

  if (!ctx->defer_display_setup)
    {
#if 0
      if (argc && *argc > 0 && *argv)
	g_set_prgname ((*argv)[0]);
#endif

      context = g_option_context_new (parameter_string);

      group = clutter_get_option_group ();
      g_option_context_add_group (context, group);

      group = cogl_get_option_group ();
      g_option_context_add_group (context, group);

      if (entries)
	g_option_context_add_main_entries (context, entries, translation_domain);

      res = g_option_context_parse (context, argc, argv, error);
      g_option_context_free (context);

      /* if res is FALSE, the error is filled for
       * us by g_option_context_parse()
       */
      if (!res)
	{
	  /* if there has been an error in the initialization, the
	   * error id will be preserved inside the GError code
	   */
	  if (error && *error)
	    return (*error)->code;
	  else
	    return CLUTTER_INIT_ERROR_INTERNAL;
	}

      return CLUTTER_INIT_SUCCESS;
    }
  else
    return clutter_init_real (error);
}

static gboolean
clutter_parse_args (int    *argc,
                    char ***argv)
{
  GOptionContext *option_context;
  GOptionGroup   *clutter_group, *cogl_group;
  GError         *error = NULL;
  gboolean        ret = TRUE;

  if (clutter_is_initialized)
    return TRUE;

  option_context = g_option_context_new (NULL);
  g_option_context_set_ignore_unknown_options (option_context, TRUE);
  g_option_context_set_help_enabled (option_context, FALSE);

  /* Initiate any command line options from the backend */

  clutter_group = clutter_get_option_group ();
  g_option_context_set_main_group (option_context, clutter_group);

  cogl_group = cogl_get_option_group ();
  g_option_context_add_group (option_context, cogl_group);

  if (!g_option_context_parse (option_context, argc, argv, &error))
    {
      if (error)
	{
	  g_warning ("%s", error->message);
	  g_error_free (error);
	}

      ret = FALSE;
    }

  g_option_context_free (option_context);

  return ret;
}

/**
 * clutter_init:
 * @argc: (inout): The number of arguments in @argv
 * @argv: (array length=argc) (inout) (allow-none): A pointer to an array
 *   of arguments.
 *
 * It will initialise everything needed to operate with Clutter and
 * parses some standard command line options. @argc and @argv are
 * adjusted accordingly so your own code will never see those standard
 * arguments.
 *
 * Return value: 1 on success, < 0 on failure.
 */
ClutterInitError
clutter_init (int    *argc,
              char ***argv)
{
  ClutterMainContext *ctx;
  GError *error = NULL;

  if (clutter_is_initialized)
    return CLUTTER_INIT_SUCCESS;

  clutter_base_init ();

  ctx = _clutter_context_get_default ();

  if (!ctx->defer_display_setup)
    {
#if 0
      if (argc && *argc > 0 && *argv)
	g_set_prgname ((*argv)[0]);
#endif

      /* parse_args will trigger backend creation and things like
       * DISPLAY connection etc.
       */
      if (clutter_parse_args (argc, argv) == FALSE)
	{
	  CLUTTER_NOTE (MISC, "failed to parse arguments.");
	  return CLUTTER_INIT_ERROR_INTERNAL;
	}

      return CLUTTER_INIT_SUCCESS;
    }
  else
    return clutter_init_real (&error);
}

gboolean
_clutter_boolean_handled_accumulator (GSignalInvocationHint *ihint,
                                      GValue                *return_accu,
                                      const GValue          *handler_return,
                                      gpointer               dummy)
{
  gboolean continue_emission;
  gboolean signal_handled;

  signal_handled = g_value_get_boolean (handler_return);
  g_value_set_boolean (return_accu, signal_handled);
  continue_emission = !signal_handled;

  return continue_emission;
}

static void
event_click_count_generate (ClutterEvent *event)
{
  /* multiple button click detection */
  static gint    click_count            = 0;
  static gint    previous_x             = -1;
  static gint    previous_y             = -1;
  static guint32 previous_time          = 0;
  static gint    previous_button_number = -1;

  ClutterInputDevice *device = NULL;
  ClutterBackend *backend;
  guint double_click_time;
  guint double_click_distance;

  backend = clutter_get_default_backend ();
  double_click_distance = clutter_backend_get_double_click_distance (backend);
  double_click_time = clutter_backend_get_double_click_time (backend);

  device = clutter_event_get_device (event);
  if (device != NULL)
    {
      click_count = device->click_count;
      previous_x = device->previous_x;
      previous_y = device->previous_y;
      previous_time = device->previous_time;
      previous_button_number = device->previous_button_number;

      CLUTTER_NOTE (EVENT,
                    "Restoring previous click count:%d (device:%d, time:%u)",
                    click_count,
                    clutter_input_device_get_device_id (device),
                    previous_time);
    }
  else
    {
      CLUTTER_NOTE (EVENT,
                    "Restoring previous click count:%d (time:%u)",
                    click_count,
                    previous_time);
    }

  switch (clutter_event_type (event))
    {
      case CLUTTER_BUTTON_PRESS:
        /* check if we are in time and within distance to increment an
         * existing click count
         */
        if (event->button.button == previous_button_number &&
            event->button.time < (previous_time + double_click_time) &&
            (ABS (event->button.x - previous_x) <= double_click_distance) &&
            (ABS (event->button.y - previous_y) <= double_click_distance))
          {
            CLUTTER_NOTE (EVENT, "Increase click count (button: %d, time: %u)",
                          event->button.button,
                          event->button.time);

            click_count += 1;
          }
        else /* start a new click count*/
          {
            CLUTTER_NOTE (EVENT, "Reset click count (button: %d, time: %u)",
                          event->button.button,
                          event->button.time);

            click_count = 1;
            previous_button_number = event->button.button;
          }

        previous_x = event->button.x;
        previous_y = event->button.y;
        previous_time = event->button.time;

        /* fallthrough */
      case CLUTTER_BUTTON_RELEASE:
        event->button.click_count = click_count;
        break;

      default:
        g_assert_not_reached ();
        break;
    }

  if (event->type == CLUTTER_BUTTON_PRESS && device != NULL)
    {
      CLUTTER_NOTE (EVENT, "Storing click count: %d (device:%d, time:%u)",
                    click_count,
                    clutter_input_device_get_device_id (device),
                    previous_time);

      device->click_count = click_count;
      device->previous_x = previous_x;
      device->previous_y = previous_y;
      device->previous_time = previous_time;
      device->previous_button_number = previous_button_number;
    }
}

static inline void
emit_event (ClutterEvent *event,
            gboolean      is_key_event)
{
  static gboolean      lock = FALSE;

  GPtrArray *event_tree = NULL;
  ClutterActor *actor;
  gint i = 0;

  if (event->any.source == NULL)
    {
      CLUTTER_NOTE (EVENT, "No source set, discarding event");
      return;
    }

  /* reentrancy check */
  if (lock != FALSE)
    {
      g_warning ("Tried emitting event during event delivery, bailing out.n");
      return;
    }

  lock = TRUE;

  event_tree = g_ptr_array_sized_new (64);

  actor = event->any.source;

  /* Build 'tree' of emitters for the event */
  while (actor)
    {
      ClutterActor *parent;

      parent = clutter_actor_get_parent (actor);

      if (clutter_actor_get_reactive (actor) ||
          parent == NULL ||         /* stage gets all events */
          is_key_event)             /* keyboard events are always emitted */
        {
          g_ptr_array_add (event_tree, g_object_ref (actor));
        }

      actor = parent;
    }

  /* Capture */
  for (i = event_tree->len - 1; i >= 0; i--)
    if (clutter_actor_event (g_ptr_array_index (event_tree, i), event, TRUE))
      goto done;

  /* Bubble */
  for (i = 0; i < event_tree->len; i++)
    if (clutter_actor_event (g_ptr_array_index (event_tree, i), event, FALSE))
      goto done;

done:
  for (i = 0; i < event_tree->len; i++)
    g_object_unref (g_ptr_array_index (event_tree, i));

  g_ptr_array_free (event_tree, TRUE);

  lock = FALSE;
}

/*
 * Emits a pointer event after having prepared the event for delivery (setting
 * source, computing click_count, generating enter/leave etc.).
 */

static inline void
emit_pointer_event (ClutterEvent       *event,
                    ClutterInputDevice *device)
{
  ClutterMainContext *context = _clutter_context_get_default ();

  if (context->pointer_grab_actor == NULL &&
      (device == NULL || device->pointer_grab_actor == NULL))
    {
      /* no grab, time to capture and bubble */
      emit_event (event, FALSE);
    }
  else
    {
      if (context->pointer_grab_actor != NULL)
        {
          /* global grab */
          clutter_actor_event (context->pointer_grab_actor, event, FALSE);
        }
      else if (device != NULL && device->pointer_grab_actor != NULL)
        {
          /* per device grab */
          clutter_actor_event (device->pointer_grab_actor, event, FALSE);
        }
    }
}

static inline void
emit_keyboard_event (ClutterEvent *event)
{
  ClutterMainContext *context = _clutter_context_get_default ();

  if (context->keyboard_grab_actor == NULL)
    emit_event (event, TRUE);
  else
    clutter_actor_event (context->keyboard_grab_actor, event, FALSE);
}

static gboolean
is_off_stage (ClutterActor *stage,
              gfloat        x,
              gfloat        y)
{
  return (x < 0 ||
          y < 0 ||
          x >= clutter_actor_get_width (stage) ||
          y >= clutter_actor_get_height (stage));
}

/**
 * clutter_do_event
 * @event: a #ClutterEvent.
 *
 * Processes an event. This function should never be called by applications.
 *
 * Since: 0.4
 */
void
clutter_do_event (ClutterEvent *event)
{
  if (!event->any.stage)
    return;

  /* Instead of processing events when received, we queue them up to
   * handle per-frame before animations, layout, and drawing.
   *
   * This gives us the chance to reliably compress motion events
   * because we've "looked ahead" and know all motion events that
   * will occur before drawing the frame.
   */
  _clutter_stage_queue_event (event->any.stage, event);
}

static void
_clutter_process_event_details (ClutterActor        *stage,
                                ClutterMainContext  *context,
                                ClutterEvent        *event)
{
  ClutterInputDevice *device = NULL;

  device = clutter_event_get_device (event);

  switch (event->type)
    {
      case CLUTTER_NOTHING:
        event->any.source = stage;
        break;

      case CLUTTER_LEAVE:
      case CLUTTER_ENTER:
        emit_pointer_event (event, device);
        break;

      case CLUTTER_DESTROY_NOTIFY:
      case CLUTTER_DELETE:
        event->any.source = stage;
        /* the stage did not handle the event, so we just quit */
        clutter_stage_event (CLUTTER_STAGE (stage), event);
        break;

      case CLUTTER_KEY_PRESS:
      case CLUTTER_KEY_RELEASE:
        {
          ClutterActor *actor = NULL;

          /* check that we're not a synthetic event with source set */
          if (event->any.source == NULL)
            {
              actor = clutter_stage_get_key_focus (CLUTTER_STAGE (stage));
              event->any.source = actor;
              if (G_UNLIKELY (actor == NULL))
                {
                  g_warning ("No key focus set, discarding");
                  return;
                }
            }

          emit_keyboard_event (event);
        }
        break;

      case CLUTTER_MOTION:
        /* Only stage gets motion events if clutter_set_motion_events is TRUE,
         * and the event is not a synthetic event with source set.
         */
        if (!context->motion_events_per_actor &&
            event->any.source == NULL)
          {
            /* Only stage gets motion events */
            event->any.source = stage;

            /* global grabs */
            if (context->pointer_grab_actor != NULL)
              {
                clutter_actor_event (context->pointer_grab_actor,
                                     event, FALSE);
                break;
              }
            else if (device != NULL && device->pointer_grab_actor != NULL)
              {
                clutter_actor_event (device->pointer_grab_actor,
                                     event, FALSE);
                break;
              }

            /* Trigger handlers on stage in both capture .. */
            if (!clutter_actor_event (stage, event, TRUE))
              {
                /* and bubbling phase */
                clutter_actor_event (stage, event, FALSE);
              }
            break;
          }

      /* fallthrough from motion */
      case CLUTTER_BUTTON_PRESS:
      case CLUTTER_BUTTON_RELEASE:
      case CLUTTER_SCROLL:
        {
          ClutterActor *actor;
          gfloat x, y;

          clutter_event_get_coords (event, &x, &y);

          /* Only do a pick to find the source if source is not already set
           * (as it could be in a synthetic event)
           */
          if (event->any.source == NULL)
            {
              /* emulate X11 the implicit soft grab; the implicit soft grab
               * keeps relaying motion events when the stage is left with a
               * pointer button pressed. since this is what happens when we
               * disable per-actor motion events we need to maintain the same
               * behaviour when the per-actor motion events are enabled as
               * well
               */
              if (is_off_stage (stage, x, y))
                {
                  if (event->type == CLUTTER_BUTTON_RELEASE)
                    {
                      CLUTTER_NOTE (EVENT,
                                    "Release off stage received at %.2f, %.2f",
                                    x, y);

                      event->button.source = stage;
                      event->button.click_count = 1;

                      emit_pointer_event (event, device);
                    }
                  else if (event->type == CLUTTER_MOTION)
                    {
                      CLUTTER_NOTE (EVENT,
                                    "Motion off stage received at %.2f, %2.f",
                                    x, y);

                      event->motion.source = stage;

                      emit_pointer_event (event, device);
                    }

                  break;
                }

              /* if the backend provides a device then we should
               * already have everything we need to update it and
               * get the actor underneath
               */
              if (device != NULL)
                actor = _clutter_input_device_update (device);
              else
                {
                  CLUTTER_NOTE (EVENT, "No device found: picking");

                  actor = _clutter_do_pick (CLUTTER_STAGE (stage),
                                            x, y,
                                            CLUTTER_PICK_REACTIVE);
                }

              if (actor == NULL)
                break;

              event->any.source = actor;
            }
          else
            {
              /* use the source already set in the synthetic event */
              actor = event->any.source;
            }

          /* FIXME: for an optimisation should check if there are
           * actually any reactive actors and avoid the pick all together
           * (signalling just the stage). Should be big help for gles.
           */

          CLUTTER_NOTE (EVENT,
                        "Reactive event received at %.2f, %.2f - actor: %p",
                        x, y,
                        actor);

          /* button presses and releases need a click count */
          if (event->type == CLUTTER_BUTTON_PRESS ||
              event->type == CLUTTER_BUTTON_RELEASE)
            {
              /* Generate click count */
              event_click_count_generate (event);
            }

          emit_pointer_event (event, device);
          break;
        }

      case CLUTTER_STAGE_STATE:
        /* fullscreen / focus - forward to stage */
        event->any.source = stage;
        clutter_stage_event (CLUTTER_STAGE (stage), event);
        break;

      case CLUTTER_CLIENT_MESSAGE:
        break;
    }
}

/**
 * _clutter_process_event
 * @event: a #ClutterEvent.
 *
 * Does the actual work of processing an event that was queued earlier
 * out of clutter_do_event().
 */
void
_clutter_process_event (ClutterEvent *event)
{
  ClutterMainContext *context;
  ClutterActor *stage;

  context = _clutter_context_get_default ();

  stage = CLUTTER_ACTOR (event->any.stage);
  if (stage == NULL)
    return;

  CLUTTER_TIMESTAMP (EVENT, "Event received");

  context->last_event_time = clutter_event_get_time (event);

  context->current_event = event;
  _clutter_process_event_details (stage, context, event);
  context->current_event = NULL;
}


/**
 * clutter_get_actor_by_gid
 * @id: a #ClutterActor ID.
 *
 * Retrieves the #ClutterActor with @id.
 *
 * Return value: (transfer none): the actor with the passed id or %NULL.
 *   The returned actor does not have its reference count increased.
 *
 * Since: 0.6
 */
ClutterActor*
clutter_get_actor_by_gid (guint32 id)
{
  ClutterMainContext *context;

  context = _clutter_context_get_default ();

  g_return_val_if_fail (context != NULL, NULL);

  return CLUTTER_ACTOR (clutter_id_pool_lookup (context->id_pool, id));
}

void
clutter_base_init (void)
{
  static gboolean initialised = FALSE;

  if (!initialised)
    {
      initialised = TRUE;

      bindtextdomain (GETTEXT_PACKAGE, CLUTTER_LOCALEDIR);
      bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

      /* initialise GLib type system */
      g_type_init ();
    }
}

/**
 * clutter_get_default_frame_rate:
 *
 * Retrieves the default frame rate. See clutter_set_default_frame_rate().
 *
 * Return value: the default frame rate
 *
 * Since: 0.6
 */
guint
clutter_get_default_frame_rate (void)
{
  ClutterMainContext *context;

  context = _clutter_context_get_default ();

  return context->frame_rate;
}

/**
 * clutter_set_default_frame_rate:
 * @frames_per_sec: the new default frame rate
 *
 * Sets the default frame rate. This frame rate will be used to limit
 * the number of frames drawn if Clutter is not able to synchronize
 * with the vertical refresh rate of the display. When synchronization
 * is possible, this value is ignored.
 *
 * Since: 0.6
 */
void
clutter_set_default_frame_rate (guint frames_per_sec)
{
  ClutterMainContext *context;

  context = _clutter_context_get_default ();

  if (context->frame_rate != frames_per_sec)
    context->frame_rate = frames_per_sec;
}


static void
on_pointer_grab_weak_notify (gpointer data,
                             GObject *where_the_object_was)
{
  ClutterInputDevice *dev = (ClutterInputDevice *)data;
  ClutterMainContext *context;

  context = _clutter_context_get_default ();

  if (dev)
    {
      dev->pointer_grab_actor = NULL;
      clutter_ungrab_pointer_for_device (dev->id);
    }
  else
    {
      context->pointer_grab_actor = NULL;
      clutter_ungrab_pointer ();
    }
}

/**
 * clutter_grab_pointer:
 * @actor: a #ClutterActor
 *
 * Grabs pointer events, after the grab is done all pointer related events
 * (press, motion, release, enter, leave and scroll) are delivered to this
 * actor directly without passing through both capture and bubble phases of
 * the event delivery chain. The source set in the event will be the actor
 * that would have received the event if the pointer grab was not in effect.
 *
 * <note><para>Grabs completely override the entire event delivery chain
 * done by Clutter. Pointer grabs should only be used as a last resource;
 * using the #ClutterActor::captured-event signal should always be the
 * preferred way to intercept event delivery to reactive actors.</para></note>
 *
 * If you wish to grab all the pointer events for a specific input device,
 * you should use clutter_grab_pointer_for_device().
 *
 * Since: 0.6
 */
void
clutter_grab_pointer (ClutterActor *actor)
{
  ClutterMainContext *context;

  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  context = _clutter_context_get_default ();

  if (context->pointer_grab_actor == actor)
    return;

  if (context->pointer_grab_actor)
    {
      g_object_weak_unref (G_OBJECT (context->pointer_grab_actor),
			   on_pointer_grab_weak_notify,
			   NULL);
      context->pointer_grab_actor = NULL;
    }

  if (actor)
    {
      context->pointer_grab_actor = actor;

      g_object_weak_ref (G_OBJECT (actor),
			 on_pointer_grab_weak_notify,
			 NULL);
    }
}

/**
 * clutter_grab_pointer_for_device:
 * @actor: a #ClutterActor
 * @id: a device id, or -1
 *
 * Grabs all the pointer events coming from the device @id for @actor.
 *
 * If @id is -1 then this function is equivalent to clutter_grab_pointer().
 *
 * Since: 0.8
 */
void
clutter_grab_pointer_for_device (ClutterActor *actor,
                                 gint          id)
{
  ClutterInputDevice *dev;

  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  /* essentially a global grab */
  if (id == -1)
    {
      clutter_grab_pointer (actor);
      return;
    }

  dev = clutter_get_input_device_for_id (id);

  if (!dev)
    return;

  if (dev->pointer_grab_actor == actor)
    return;

  if (dev->pointer_grab_actor)
    {
      g_object_weak_unref (G_OBJECT (dev->pointer_grab_actor),
                          on_pointer_grab_weak_notify,
                          dev);
      dev->pointer_grab_actor = NULL;
    }

  if (actor)
    {
      dev->pointer_grab_actor = actor;

      g_object_weak_ref (G_OBJECT (actor),
                        on_pointer_grab_weak_notify,
                        dev);
    }
}


/**
 * clutter_ungrab_pointer:
 *
 * Removes an existing grab of the pointer.
 *
 * Since: 0.6
 */
void
clutter_ungrab_pointer (void)
{
  clutter_grab_pointer (NULL);
}

/**
 * clutter_ungrab_pointer_for_device:
 * @id: a device id
 *
 * Removes an existing grab of the pointer events for device @id.
 *
 * Since: 0.8
 */
void
clutter_ungrab_pointer_for_device (gint id)
{
  clutter_grab_pointer_for_device (NULL, id);
}


/**
 * clutter_get_pointer_grab:
 *
 * Queries the current pointer grab of clutter.
 *
 * Return value: (transfer none): the actor currently holding the pointer grab, or NULL if there is no grab.
 *
 * Since: 0.6
 */
ClutterActor *
clutter_get_pointer_grab (void)
{
  ClutterMainContext *context;
  context = _clutter_context_get_default ();

  return context->pointer_grab_actor;
}


static void
on_keyboard_grab_weak_notify (gpointer data,
                              GObject *where_the_object_was)
{
  ClutterMainContext *context;

  context = _clutter_context_get_default ();
  context->keyboard_grab_actor = NULL;

  clutter_ungrab_keyboard ();
}

/**
 * clutter_grab_keyboard:
 * @actor: a #ClutterActor
 *
 * Grabs keyboard events, after the grab is done keyboard
 * events (#ClutterActor::key-press-event and #ClutterActor::key-release-event)
 * are delivered to this actor directly. The source set in the event will be
 * the actor that would have received the event if the keyboard grab was not
 * in effect.
 *
 * Like pointer grabs, keyboard grabs should only be used as a last
 * resource.
 *
 * See also clutter_stage_set_key_focus() and clutter_actor_grab_key_focus()
 * to perform a "soft" key grab and assign key focus to a specific actor.
 *
 * Since: 0.6
 */
void
clutter_grab_keyboard (ClutterActor *actor)
{
  ClutterMainContext *context;

  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  context = _clutter_context_get_default ();

  if (context->keyboard_grab_actor == actor)
    return;

  if (context->keyboard_grab_actor)
    {
      g_object_weak_unref (G_OBJECT (context->keyboard_grab_actor),
			   on_keyboard_grab_weak_notify,
			   NULL);
      context->keyboard_grab_actor = NULL;
    }

  if (actor)
    {
      context->keyboard_grab_actor = actor;

      g_object_weak_ref (G_OBJECT (actor),
			 on_keyboard_grab_weak_notify,
			 NULL);
    }
}

/**
 * clutter_ungrab_keyboard:
 *
 * Removes an existing grab of the keyboard.
 *
 * Since: 0.6
 */
void
clutter_ungrab_keyboard (void)
{
  clutter_grab_keyboard (NULL);
}

/**
 * clutter_get_keyboard_grab:
 *
 * Queries the current keyboard grab of clutter.
 *
 * Return value: (transfer none): the actor currently holding the keyboard grab, or NULL if there is no grab.
 *
 * Since: 0.6
 */
ClutterActor *
clutter_get_keyboard_grab (void)
{
  ClutterMainContext *context;

  context = _clutter_context_get_default ();

  return context->keyboard_grab_actor;
}

/**
 * clutter_clear_glyph_cache:
 *
 * Clears the internal cache of glyphs used by the Pango
 * renderer. This will free up some memory and GL texture
 * resources. The cache will be automatically refilled as more text is
 * drawn.
 *
 * Since: 0.8
 */
void
clutter_clear_glyph_cache (void)
{
  CoglPangoFontMap *font_map;

  font_map = _clutter_context_get_pango_fontmap (CLUTTER_CONTEXT ());
  cogl_pango_font_map_clear_glyph_cache (font_map);
}

/**
 * clutter_set_font_flags:
 * @flags: The new flags
 *
 * Sets the font quality options for subsequent text rendering
 * operations.
 *
 * Using mipmapped textures will improve the quality for scaled down
 * text but will use more texture memory.
 *
 * Enabling hinting improves text quality for static text but may
 * introduce some artifacts if the text is animated.
 *
 * Since: 1.0
 */
void
clutter_set_font_flags (ClutterFontFlags flags)
{
  ClutterMainContext *context = _clutter_context_get_default ();
  CoglPangoFontMap *font_map;
  ClutterFontFlags old_flags, changed_flags;
  const cairo_font_options_t *font_options;
  cairo_font_options_t *new_font_options;
  gboolean use_mipmapping;
  ClutterBackend *backend;

  backend = clutter_get_default_backend ();

  font_map = _clutter_context_get_pango_fontmap (context);
  use_mipmapping = (flags & CLUTTER_FONT_MIPMAPPING) != 0;
  cogl_pango_font_map_set_use_mipmapping (font_map, use_mipmapping);

  old_flags = clutter_get_font_flags ();

  font_options = clutter_backend_get_font_options (backend);
  new_font_options = cairo_font_options_copy (font_options);

  /* Only set the font options that have actually changed so we don't
     override a detailed setting from the backend */
  changed_flags = old_flags ^ flags;

  if ((changed_flags & CLUTTER_FONT_HINTING))
    cairo_font_options_set_hint_style (new_font_options,
                                       (flags & CLUTTER_FONT_HINTING)
                                       ? CAIRO_HINT_STYLE_FULL
                                       : CAIRO_HINT_STYLE_NONE);

  clutter_backend_set_font_options (backend, new_font_options);

  cairo_font_options_destroy (new_font_options);

  /* update the default pango context, if any */
  if (context->pango_context != NULL)
    update_pango_context (backend, context->pango_context);
}

/**
 * clutter_get_font_flags:
 *
 * Gets the current font flags for rendering text. See
 * clutter_set_font_flags().
 *
 * Return value: The font flags
 *
 * Since: 1.0
 */
ClutterFontFlags
clutter_get_font_flags (void)
{
  ClutterMainContext *context = CLUTTER_CONTEXT ();
  CoglPangoFontMap *font_map = NULL;
  const cairo_font_options_t *font_options;
  ClutterFontFlags flags = 0;

  font_map = _clutter_context_get_pango_fontmap (context);
  if (cogl_pango_font_map_get_use_mipmapping (font_map))
    flags |= CLUTTER_FONT_MIPMAPPING;

  font_options = clutter_backend_get_font_options (context->backend);

  if ((cairo_font_options_get_hint_style (font_options)
       != CAIRO_HINT_STYLE_DEFAULT)
      && (cairo_font_options_get_hint_style (font_options)
          != CAIRO_HINT_STYLE_NONE))
    flags |= CLUTTER_FONT_HINTING;

  return flags;
}

/**
 * clutter_get_input_device_for_id:
 * @id: the unique id for a device
 *
 * Retrieves the #ClutterInputDevice from its @id. This is a convenience
 * wrapper for clutter_device_manager_get_device() and it is functionally
 * equivalent to:
 *
 * |[
 *   ClutterDeviceManager *manager;
 *   ClutterInputDevice *device;
 *
 *   manager = clutter_device_manager_get_default ();
 *   device = clutter_device_manager_get_device (manager, id);
 * ]|
 *
 * Return value: (transfer none): a #ClutterInputDevice, or %NULL
 *
 * Since: 0.8
 */
ClutterInputDevice *
clutter_get_input_device_for_id (gint id)
{
  ClutterDeviceManager *manager;

  manager = clutter_device_manager_get_default ();

  return clutter_device_manager_get_device (manager, id);
}

/**
 * clutter_get_font_map:
 *
 * Retrieves the #PangoFontMap instance used by Clutter.
 * You can use the global font map object with the COGL
 * Pango API.
 *
 * Return value: (transfer none): the #PangoFontMap instance. The returned
 *   value is owned by Clutter and it should never be unreferenced.
 *
 * Since: 1.0
 */
PangoFontMap *
clutter_get_font_map (void)
{
  ClutterMainContext *context = _clutter_context_get_default ();

  return PANGO_FONT_MAP (_clutter_context_get_pango_fontmap (context));
}

typedef struct _ClutterRepaintFunction
{
  guint id;
  GSourceFunc func;
  gpointer data;
  GDestroyNotify notify;
} ClutterRepaintFunction;

/**
 * clutter_threads_remove_repaint_func:
 * @handle_id: an unsigned integer greater than zero
 *
 * Removes the repaint function with @handle_id as its id
 *
 * Since: 1.0
 */
void
clutter_threads_remove_repaint_func (guint handle_id)
{
  ClutterRepaintFunction *repaint_func;
  ClutterMainContext *context;
  GList *l;

  g_return_if_fail (handle_id > 0);

  context = CLUTTER_CONTEXT ();
  l = context->repaint_funcs;
  while (l != NULL)
    {
      repaint_func = l->data;

      if (repaint_func->id == handle_id)
        {
          context->repaint_funcs =
            g_list_remove_link (context->repaint_funcs, l);

          g_list_free (l);

          if (repaint_func->notify)
            repaint_func->notify (repaint_func->data);

          g_slice_free (ClutterRepaintFunction, repaint_func);

          return;
        }

      l = l->next;
    }
}

/**
 * clutter_threads_add_repaint_func:
 * @func: the function to be called within the paint cycle
 * @data: data to be passed to the function, or %NULL
 * @notify: function to be called when removing the repaint
 *    function, or %NULL
 *
 * Adds a function to be called whenever Clutter is repainting a Stage.
 * If the function returns %FALSE it is automatically removed from the
 * list of repaint functions and will not be called again.
 *
 * This function is guaranteed to be called from within the same thread
 * that called clutter_main(), and while the Clutter lock is being held.
 *
 * A repaint function is useful to ensure that an update of the scenegraph
 * is performed before the scenegraph is repainted; for instance, uploading
 * a frame from a video into a #ClutterTexture.
 *
 * When the repaint function is removed (either because it returned %FALSE
 * or because clutter_threads_remove_repaint_func() has been called) the
 * @notify function will be called, if any is set.
 *
 * Return value: the ID (greater than 0) of the repaint function. You
 *   can use the returned integer to remove the repaint function by
 *   calling clutter_threads_remove_repaint_func().
 *
 * Since: 1.0
 */
guint
clutter_threads_add_repaint_func (GSourceFunc    func,
                                  gpointer       data,
                                  GDestroyNotify notify)
{
  static guint repaint_id = 1;
  ClutterMainContext *context;
  ClutterRepaintFunction *repaint_func;

  g_return_val_if_fail (func != NULL, 0);

  context = CLUTTER_CONTEXT ();

  /* XXX lock the context */

  repaint_func = g_slice_new (ClutterRepaintFunction);

  repaint_func->id = repaint_id++;
  repaint_func->func = func;
  repaint_func->data = data;
  repaint_func->notify = notify;

  context->repaint_funcs = g_list_prepend (context->repaint_funcs,
                                           repaint_func);

  /* XXX unlock the context */

  return repaint_func->id;
}

/*
 * _clutter_run_repaint_functions:
 *
 * Executes the repaint functions added using the
 * clutter_threads_add_repaint_func() function.
 *
 * Must be called before calling clutter_redraw() and
 * with the Clutter thread lock held.
 */
void
_clutter_run_repaint_functions (void)
{
  ClutterMainContext *context = CLUTTER_CONTEXT ();
  ClutterRepaintFunction *repaint_func;
  GList *reinvoke_list, *l;

  if (context->repaint_funcs == NULL)
    return;

  reinvoke_list = NULL;

  /* consume the whole list while we execute the functions */
  while (context->repaint_funcs)
    {
      gboolean res = FALSE;

      repaint_func = context->repaint_funcs->data;

      l = context->repaint_funcs;
      context->repaint_funcs =
        g_list_remove_link (context->repaint_funcs, context->repaint_funcs);

      g_list_free (l);

      res = repaint_func->func (repaint_func->data);

      if (res)
        reinvoke_list = g_list_prepend (reinvoke_list, repaint_func);
      else
        {
          if (repaint_func->notify)
            repaint_func->notify (repaint_func->data);

          g_slice_free (ClutterRepaintFunction, repaint_func);
        }
    }

  if (reinvoke_list)
    context->repaint_funcs = reinvoke_list;
}

/**
 * clutter_check_version:
 * @major: major version, like 1 in 1.2.3
 * @minor: minor version, like 2 in 1.2.3
 * @micro: micro version, like 3 in 1.2.3
 *
 * Run-time version check, to check the version the Clutter library
 * that an application is currently linked against
 *
 * This is the run-time equivalent of the compile-time %CLUTTER_CHECK_VERSION
 * pre-processor macro
 *
 * Return value: %TRUE if the version of the Clutter library is
 *   greater than (@major, @minor, @micro), and %FALSE otherwise
 *
 * Since: 1.2
 */
gboolean
clutter_check_version (guint major,
                       guint minor,
                       guint micro)
{
  return (clutter_major_version > major ||
          (clutter_major_version == major &&
           clutter_minor_version > minor) ||
          (clutter_major_version == major &&
           clutter_minor_version == minor &&
           clutter_micro_version >= micro));
}

/**
 * clutter_get_default_text_direction:
 *
 * Retrieves the default direction for the text. The text direction is
 * determined by the locale and/or by the %CLUTTER_TEXT_DIRECTION environment
 * variable
 *
 * The default text direction can be overridden on a per-actor basis by using
 * clutter_actor_set_text_direction()
 *
 * Return value: the default text direction
 *
 * Since: 1.2
 */
ClutterTextDirection
clutter_get_default_text_direction (void)
{
  return clutter_text_direction;
}
