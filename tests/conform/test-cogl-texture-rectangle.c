#include <clutter/clutter.h>
#include <string.h>

#include "test-conform-common.h"

static const ClutterColor stage_color = { 0x0, 0x0, 0x0, 0xff };

typedef struct _TestState
{
  ClutterActor *stage;
  guint frame;
} TestState;

static CoglHandle
create_source_rect (void)
{
#ifdef GL_TEXTURE_RECTANGLE_ARB

  int x, y;
  guint8 *data = g_malloc (256 * 256 * 4), *p = data;
  CoglHandle tex;
  GLuint gl_tex;

  for (y = 0; y < 256; y++)
    for (x = 0; x < 256; x++)
      {
        *(p++) = x;
        *(p++) = y;
        *(p++) = 0;
        *(p++) = 255;
      }

  glPixelStorei (GL_UNPACK_ROW_LENGTH, 256);
  glPixelStorei (GL_UNPACK_ALIGNMENT, 8);
  glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);

  glGenTextures (1, &gl_tex);
  glBindTexture (GL_TEXTURE_RECTANGLE_ARB, gl_tex);
  glTexImage2D (GL_TEXTURE_RECTANGLE_ARB, 0,
                GL_RGBA, 256, 256, 0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                data);

  g_assert (glGetError () == GL_NO_ERROR);

  g_free (data);

  tex = cogl_texture_new_from_foreign (gl_tex,
                                       GL_TEXTURE_RECTANGLE_ARB,
                                       256, 256, 0, 0,
                                       COGL_PIXEL_FORMAT_RGBA_8888);

  return tex;

#else /* GL_TEXTURE_RECTANGLE_ARB */

  return COGL_INVALID_HANDLE;

#endif /* GL_TEXTURE_RECTANGLE_ARB */
}

static CoglHandle
create_source_2d (void)
{
  int x, y;
  guint8 *data = g_malloc (256 * 256 * 4), *p = data;
  CoglHandle tex;

  for (y = 0; y < 256; y++)
    for (x = 0; x < 256; x++)
      {
        *(p++) = 0;
        *(p++) = x;
        *(p++) = y;
        *(p++) = 255;
      }

  tex = cogl_texture_new_from_data (256, 256, COGL_TEXTURE_NONE,
                                    COGL_PIXEL_FORMAT_RGBA_8888_PRE,
                                    COGL_PIXEL_FORMAT_ANY,
                                    256 * 4,
                                    data);

  g_free (data);

  return tex;
}

static void
draw_frame (TestState *state)
{
  GLuint gl_tex;
  CoglHandle tex_rect = create_source_rect ();
  CoglHandle material_rect = cogl_material_new ();
  CoglHandle tex_2d = create_source_2d ();
  CoglHandle material_2d = cogl_material_new ();

  g_assert (tex_rect != COGL_INVALID_HANDLE);

  cogl_material_set_layer (material_rect, 0, tex_rect);
  cogl_material_set_layer_filters (material_rect, 0,
                                   COGL_MATERIAL_FILTER_NEAREST,
                                   COGL_MATERIAL_FILTER_NEAREST);

  cogl_material_set_layer (material_2d, 0, tex_2d);
  cogl_material_set_layer_filters (material_2d, 0,
                                   COGL_MATERIAL_FILTER_NEAREST,
                                   COGL_MATERIAL_FILTER_NEAREST);

  cogl_set_source (material_rect);

  /* Render the texture repeated horizontally twice */
  cogl_rectangle_with_texture_coords (0.0f, 0.0f, 512.0f, 256.0f,
                                      0.0f, 0.0f, 2.0f, 1.0f);
  /* Render the top half of the texture to test without repeating */
  cogl_rectangle_with_texture_coords (0.0f, 256.0f, 256.0f, 384.0f,
                                      0.0f, 0.0f, 1.0f, 0.5f);

  cogl_set_source (material_2d);

  /* Render the top half of a regular 2D texture */
  cogl_rectangle_with_texture_coords (256.0f, 256.0f, 512.0f, 384.0f,
                                      0.0f, 0.0f, 1.0f, 0.5f);

  /* Flush the rendering now so we can safely delete the texture */
  cogl_flush ();

  cogl_handle_unref (material_rect);

  /* Cogl doesn't destroy foreign textures so we have to do it manually */
  cogl_texture_get_gl_texture (tex_rect, &gl_tex, NULL);
  glDeleteTextures (1, &gl_tex);
  cogl_handle_unref (tex_rect);
}

static void
validate_result (TestState *state)
{
  guint8 *data, *p;
  int x, y;

  p = data = g_malloc (512 * 256 * 4);

  cogl_read_pixels (0, 0, 512, 384,
                    COGL_READ_PIXELS_COLOR_BUFFER,
                    COGL_PIXEL_FORMAT_RGBA_8888,
                    data);

  for (y = 0; y < 384; y++)
    for (x = 0; x < 512; x++)
      {
        if (x >= 256 && y >= 256)
          {
            g_assert_cmpint (p[0], ==, 0);
            g_assert_cmpint (p[1], ==, x & 0xff);
            g_assert_cmpint (p[2], ==, y & 0xff);
          }
        else
          {
            g_assert_cmpint (p[0], ==, x & 0xff);
            g_assert_cmpint (p[1], ==, y & 0xff);
            g_assert_cmpint (p[2], ==, 0);
          }
        p += 4;
      }

  g_free (data);

  /* Comment this out to see what the test paints */
  clutter_main_quit ();
}

static void
on_paint (ClutterActor *actor, TestState *state)
{
  int frame_num;

  draw_frame (state);

  /* XXX: Experiments have shown that for some buggy drivers, when using
   * glReadPixels there is some kind of race, so we delay our test for a
   * few frames and a few seconds:
   */
  /* Need to increment frame first because clutter_stage_read_pixels
     fires a redraw */
  frame_num = state->frame++;
  if (frame_num == 2)
    validate_result (state);
  else if (frame_num < 2)
    g_usleep (G_USEC_PER_SEC);
}

static gboolean
queue_redraw (gpointer stage)
{
  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  return TRUE;
}

static gboolean
check_rectangle_extension (void)
{
  static const char rect_extension[] = "GL_ARB_texture_rectangle";
  const char *extensions = (const char *) glGetString (GL_EXTENSIONS);
  const char *extensions_end;

  extensions_end = extensions + strlen (extensions);

  while (extensions < extensions_end)
    {
      const char *end = strchr (extensions, ' ');

      if (end == NULL)
        end = extensions_end;

      if (end - extensions == sizeof (rect_extension) - 1 &&
          !memcmp (extensions, rect_extension, sizeof (rect_extension) - 1))
        return TRUE;

      extensions = end + 1;
    }

  return FALSE;
}

void
test_cogl_texture_rectangle (TestConformSimpleFixture *fixture,
                             gconstpointer data)
{
  TestState state;
  guint idle_source;
  guint paint_handler;

  state.frame = 0;

  state.stage = clutter_stage_get_default ();

  /* Check whether GL supports the rectangle extension. If not we'll
     just assume the test passes */
  if (check_rectangle_extension ())
    {
      clutter_stage_set_color (CLUTTER_STAGE (state.stage), &stage_color);

      /* We force continuous redrawing of the stage, since we need to skip
       * the first few frames, and we wont be doing anything else that
       * will trigger redrawing. */
      idle_source = g_idle_add (queue_redraw, state.stage);

      paint_handler = g_signal_connect_after (state.stage, "paint",
                                              G_CALLBACK (on_paint), &state);

      clutter_actor_show_all (state.stage);

      clutter_main ();

      g_source_remove (idle_source);
      g_signal_handler_disconnect (state.stage, paint_handler);

      if (g_test_verbose ())
        g_print ("OK\n");
    }
  else if (g_test_verbose ())
    g_print ("Skipping\n");
}
