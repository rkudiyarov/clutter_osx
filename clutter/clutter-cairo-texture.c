/*
 * Clutter
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By: Emmanuele Bassi <ebassi@linux.intel.com>
 *              Matthew Allum <mallum@o-hand.com>
 *              Chris Lord <chris@o-hand.com>
 *              Iain Holmes <iain@o-hand.com>
 *              Neil Roberts <neil@linux.intel.com>
 *
 * Copyright (C) 2008, 2009, 2010  Intel Corporation.
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
 */

/**
 * SECTION:clutter-cairo-texture
 * @short_description: Texture with Cairo integration
 *
 * #ClutterCairoTexture is a #ClutterTexture that displays the contents
 * of a Cairo context. The #ClutterCairoTexture actor will create a
 * Cairo image surface which will then be uploaded to a GL texture when
 * needed.
 *
 * #ClutterCairoTexture will provide a #cairo_t context by using the
 * clutter_cairo_texture_create() and clutter_cairo_texture_create_region()
 * functions; you can use the Cairo API to draw on the context and then
 * call cairo_destroy() when done.
 *
 * As soon as the context is destroyed with cairo_destroy(), the contents
 * of the surface will be uploaded into the #ClutterCairoTexture actor:
 *
 * |[
 *   cairo_t *cr;
 *
 *   cr = clutter_cairo_texture_create (CLUTTER_CAIRO_TEXTURE (texture));
 *
 *   /&ast; draw on the context &ast;/
 *
 *   cairo_destroy (cr);
 * ]|
 *
 * Although a new #cairo_t is created each time you call
 * clutter_cairo_texture_create() or
 * clutter_cairo_texture_create_region(), it uses the same
 * #cairo_surface_t each time. You can call
 * clutter_cairo_texture_clear() to erase the contents between calls.
 *
 * <warning><para>Note that you should never use the code above inside the
 * #ClutterActor::paint or #ClutterActor::pick virtual functions or
 * signal handlers because it will lead to performance
 * degradation.</para></warning>
 *
 * <note><para>Since #ClutterCairoTexture uses a Cairo image surface
 * internally all the drawing operations will be performed in
 * software and not using hardware acceleration. This can lead to
 * performance degradation if the contents of the texture change
 * frequently.</para></note>
 *
 * #ClutterCairoTexture is available since Clutter 1.0.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "clutter-cairo-texture.h"
#include "clutter-debug.h"
#include "clutter-private.h"

G_DEFINE_TYPE (ClutterCairoTexture,
               clutter_cairo_texture,
               CLUTTER_TYPE_TEXTURE);

enum
{
  PROP_0,

  PROP_SURFACE_WIDTH,
  PROP_SURFACE_HEIGHT
};

#ifdef CLUTTER_ENABLE_DEBUG
#define clutter_warn_if_paint_fail(obj)                 G_STMT_START {  \
  if (CLUTTER_ACTOR_IN_PAINT (obj)) {                                   \
    g_warning ("%s should not be called during the paint sequence "     \
               "of a ClutterCairoTexture as it will likely cause "      \
               "performance issues.", G_STRFUNC);                       \
  }                                                     } G_STMT_END
#else
#define clutter_warn_if_paint_fail(obj)         /* void */
#endif /* CLUTTER_ENABLE_DEBUG */

#define CLUTTER_CAIRO_TEXTURE_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), CLUTTER_TYPE_CAIRO_TEXTURE, ClutterCairoTexturePrivate))

/* Cairo stores the data in native byte order as ARGB but Cogl's pixel
   formats specify the actual byte order. Therefore we need to use a
   different format depending on the architecture */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define CLUTTER_CAIRO_TEXTURE_PIXEL_FORMAT COGL_PIXEL_FORMAT_BGRA_8888_PRE
#else
#define CLUTTER_CAIRO_TEXTURE_PIXEL_FORMAT COGL_PIXEL_FORMAT_ARGB_8888_PRE
#endif

struct _ClutterCairoTexturePrivate
{
  cairo_format_t   format;

  cairo_surface_t *cr_surface;
  guchar          *cr_surface_data;

  guint            width;
  guint            height;
  guint            rowstride;
};

typedef struct
{
  gint x;
  gint y;
  guint width;
  guint height;
} ClutterCairoTextureRectangle;

typedef struct
{
  ClutterCairoTexture *cairo;
  ClutterCairoTextureRectangle rect;
} ClutterCairoTextureContext;

static const cairo_user_data_key_t clutter_cairo_texture_surface_key;
static const cairo_user_data_key_t clutter_cairo_texture_context_key;

static void
clutter_cairo_texture_surface_destroy (void *data)
{
  ClutterCairoTexture *cairo = data;

  cairo->priv->cr_surface = NULL;
}

static void
clutter_cairo_texture_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ClutterCairoTexturePrivate *priv;

  priv = CLUTTER_CAIRO_TEXTURE (object)->priv;

  switch (prop_id)
    {
    case PROP_SURFACE_WIDTH:
      priv->width = g_value_get_uint (value);
      break;

    case PROP_SURFACE_HEIGHT:
      priv->height = g_value_get_uint (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clutter_cairo_texture_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ClutterCairoTexturePrivate *priv;

  priv = CLUTTER_CAIRO_TEXTURE (object)->priv;

  switch (prop_id)
    {
    case PROP_SURFACE_WIDTH:
      g_value_set_uint (value, priv->width);
      break;

    case PROP_SURFACE_HEIGHT:
      g_value_set_uint (value, priv->height);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
clutter_cairo_texture_finalize (GObject *object)
{
  ClutterCairoTexturePrivate *priv = CLUTTER_CAIRO_TEXTURE (object)->priv;

  if (priv->cr_surface)
    {
      cairo_surface_t *surface = priv->cr_surface;

      cairo_surface_finish (priv->cr_surface);
      cairo_surface_set_user_data (priv->cr_surface,
                                   &clutter_cairo_texture_surface_key,
                                   NULL, NULL);
      cairo_surface_destroy (surface);

      priv->cr_surface = NULL;
    }

  if (priv->cr_surface_data)
    {
      g_free (priv->cr_surface_data);
      priv->cr_surface_data = NULL;
    }

  G_OBJECT_CLASS (clutter_cairo_texture_parent_class)->finalize (object);
}

static inline void
clutter_cairo_texture_surface_resize_internal (ClutterCairoTexture *cairo)
{
  ClutterCairoTexturePrivate *priv = cairo->priv;
  CoglHandle cogl_texture;

  if (priv->cr_surface)
    {
      cairo_surface_t *surface = priv->cr_surface;

      /* If the surface is already the right size then don't bother
         doing anything */
      if (priv->width == cairo_image_surface_get_width (priv->cr_surface)
          && priv->height == cairo_image_surface_get_height (priv->cr_surface))
        return;

      cairo_surface_finish (surface);
      cairo_surface_set_user_data (surface,
                                   &clutter_cairo_texture_surface_key,
				   NULL, NULL);
      cairo_surface_destroy (surface);

      priv->cr_surface = NULL;
    }

  if (priv->cr_surface_data)
    {
      g_free (priv->cr_surface_data);
      priv->cr_surface_data = NULL;
    }

  if (priv->width == 0 || priv->height == 0)
    return;

#if CAIRO_VERSION > 106000
  priv->rowstride = cairo_format_stride_for_width (priv->format, priv->width);
#else
  /* poor man's version of cairo_format_stride_for_width() */
  switch (priv->format)
    {
    case CAIRO_FORMAT_ARGB32:
    case CAIRO_FORMAT_RGB24:
      priv->rowstride = priv->width * 4;
      break;

    case CAIRO_FORMAT_A8:
    case CAIRO_FORMAT_A1:
      priv->rowstride = priv->width;
      break;

    default:
      g_assert_not_reached ();
      break;
    }
#endif /* CAIRO_VERSION > 106000 */

  priv->cr_surface_data = g_malloc0 (priv->height * priv->rowstride);
  priv->cr_surface =
    cairo_image_surface_create_for_data (priv->cr_surface_data,
                                         priv->format,
                                         priv->width, priv->height,
                                         priv->rowstride);

  cairo_surface_set_user_data (priv->cr_surface,
                               &clutter_cairo_texture_surface_key,
			       cairo,
                               clutter_cairo_texture_surface_destroy);

  /* Create a blank Cogl texture */
  cogl_texture = cogl_texture_new_from_data (priv->width, priv->height,
                                             COGL_TEXTURE_NONE,
                                             CLUTTER_CAIRO_TEXTURE_PIXEL_FORMAT,
                                             COGL_PIXEL_FORMAT_ANY,
                                             priv->rowstride,
                                             priv->cr_surface_data);
  clutter_texture_set_cogl_texture (CLUTTER_TEXTURE (cairo), cogl_texture);
  cogl_handle_unref (cogl_texture);
}

static void
clutter_cairo_texture_notify (GObject    *object,
                              GParamSpec *pspec)
{
  /* When the surface width or height changes then resize the cairo
     surface. This is done here instead of directly in set_property so
     that if both the width and height properties are set using a
     single call to g_object_set then the surface will only be resized
     once because the notifications will be frozen in between */
  if (strcmp ("surface-width", pspec->name) == 0 ||
      strcmp ("surface-height", pspec->name) == 0)
    {
      ClutterCairoTexture *cairo = CLUTTER_CAIRO_TEXTURE (object);

      clutter_cairo_texture_surface_resize_internal (cairo);
    }

  if (G_OBJECT_CLASS (clutter_cairo_texture_parent_class)->notify)
    G_OBJECT_CLASS (clutter_cairo_texture_parent_class)->notify (object, pspec);
}

static void
clutter_cairo_texture_get_preferred_width (ClutterActor *actor,
                                           gfloat        for_height,
                                           gfloat       *min_width,
                                           gfloat       *natural_width)
{
  ClutterCairoTexturePrivate *priv = CLUTTER_CAIRO_TEXTURE (actor)->priv;

  if (min_width)
    *min_width = 0;

  if (natural_width)
    *natural_width = (gfloat) priv->width;
}

static void
clutter_cairo_texture_get_preferred_height (ClutterActor *actor,
                                            gfloat        for_width,
                                            gfloat       *min_height,
                                            gfloat       *natural_height)
{
  ClutterCairoTexturePrivate *priv = CLUTTER_CAIRO_TEXTURE (actor)->priv;

  if (min_height)
    *min_height = 0;

  if (natural_height)
    *natural_height = (gfloat) priv->height;
}

static void
clutter_cairo_texture_class_init (ClutterCairoTextureClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  GParamSpec *pspec;

  gobject_class->finalize     = clutter_cairo_texture_finalize;
  gobject_class->set_property = clutter_cairo_texture_set_property;
  gobject_class->get_property = clutter_cairo_texture_get_property;
  gobject_class->notify       = clutter_cairo_texture_notify;

  actor_class->get_preferred_width =
    clutter_cairo_texture_get_preferred_width;
  actor_class->get_preferred_height =
    clutter_cairo_texture_get_preferred_height;

  g_type_class_add_private (gobject_class, sizeof (ClutterCairoTexturePrivate));

  /**
   * ClutterCairoTexture:surface-width:
   *
   * The width of the Cairo surface used by the #ClutterCairoTexture
   * actor, in pixels.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_uint ("surface-width",
                             P_("Surface Width"),
                             P_("The width of the Cairo surface"),
                             0, G_MAXUINT,
                             0,
                             CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class,
                                   PROP_SURFACE_WIDTH,
                                   pspec);
  /**
   * ClutterCairoTexture:surface-height:
   *
   * The height of the Cairo surface used by the #ClutterCairoTexture
   * actor, in pixels.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_uint ("surface-height",
                             P_("Surface Height"),
                             P_("The height of the Cairo surface"),
                             0, G_MAXUINT,
                             0,
                             CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class,
                                   PROP_SURFACE_HEIGHT,
                                   pspec);
}

static void
clutter_cairo_texture_init (ClutterCairoTexture *self)
{
  ClutterCairoTexturePrivate *priv;

  self->priv = priv = CLUTTER_CAIRO_TEXTURE_GET_PRIVATE (self);

  /* FIXME - we are hardcoding the format; it would be good to have
   * a :surface-format construct-only property for creating
   * textures with a different format and have the cairo surface
   * match that format
   */
  priv->format = CAIRO_FORMAT_ARGB32;

  /* the Cairo surface is responsible for driving the size of
   * the texture; if we let sync_size to its default of TRUE,
   * the Texture will try to queue a relayout every time we
   * change the size of the Cairo surface - which is not what
   * we want
   */
  clutter_texture_set_sync_size (CLUTTER_TEXTURE (self), FALSE);
}

/**
 * clutter_cairo_texture_new:
 * @width: the width of the surface
 * @height: the height of the surface
 *
 * Creates a new #ClutterCairoTexture actor, with a surface of @width by
 * @height pixels.
 *
 * Return value: the newly created #ClutterCairoTexture actor
 *
 * Since: 1.0
 */
ClutterActor*
clutter_cairo_texture_new (guint width,
                           guint height)
{
  return g_object_new (CLUTTER_TYPE_CAIRO_TEXTURE,
                       "surface-width", width,
                       "surface-height", height,
                       NULL);
}

static void
clutter_cairo_texture_context_destroy (void *data)
{
  ClutterCairoTextureContext *ctxt = data;
  ClutterCairoTexture *cairo = ctxt->cairo;
  ClutterCairoTexturePrivate *priv = cairo->priv;
  guchar *cairo_data;
  gint cairo_width, cairo_height;
  gint surface_width, surface_height;
  CoglHandle cogl_texture;

  if (!priv->cr_surface)
    return;

  surface_width  = cairo_image_surface_get_width (priv->cr_surface);
  surface_height = cairo_image_surface_get_height (priv->cr_surface);

  cairo_width  = MIN (ctxt->rect.width, surface_width);
  cairo_height = MIN (ctxt->rect.height, surface_height);

  cogl_texture = clutter_texture_get_cogl_texture (CLUTTER_TEXTURE (cairo));

  if (!cairo_width || !cairo_height || cogl_texture == COGL_INVALID_HANDLE)
    {
      g_free (ctxt);

      return;
    }

  cairo_data = (priv->cr_surface_data
             + (ctxt->rect.y * priv->rowstride)
             + (ctxt->rect.x * 4));

  cogl_texture_set_region (cogl_texture,
                           0, 0,
                           ctxt->rect.x, ctxt->rect.y,
                           cairo_width, cairo_height,
                           cairo_width, cairo_height,
                           CLUTTER_CAIRO_TEXTURE_PIXEL_FORMAT,
                           priv->rowstride,
                           cairo_data);

  g_free (ctxt);

  clutter_actor_queue_redraw (CLUTTER_ACTOR (cairo));
}

static void
intersect_rectangles (ClutterCairoTextureRectangle *a,
		      ClutterCairoTextureRectangle *b,
		      ClutterCairoTextureRectangle *inter)
{
  gint dest_x, dest_y;
  gint dest_width, dest_height;

  dest_x = MAX (a->x, b->x);
  dest_y = MAX (a->y, b->y);
  dest_width = MIN (a->x + a->width, b->x + b->width) - dest_x;
  dest_height = MIN (a->y + a->height, b->y + b->height) - dest_y;

  if (dest_width > 0 && dest_height > 0)
    {
      inter->x = dest_x;
      inter->y = dest_y;
      inter->width = dest_width;
      inter->height = dest_height;
    }
  else
    {
      inter->x = 0;
      inter->y = 0;
      inter->width = 0;
      inter->height = 0;
    }
}

/**
 * clutter_cairo_texture_create_region:
 * @self: a #ClutterCairoTexture
 * @x_offset: offset of the region on the X axis
 * @y_offset: offset of the region on the Y axis
 * @width: width of the region, or -1 for the full surface width
 * @height: height of the region, or -1 for the full surface height
 *
 * Creates a new Cairo context that will updat the region defined
 * by @x_offset, @y_offset, @width and @height.
 *
 * <warning><para>Do not call this function within the paint virtual
 * function or from a callback to the #ClutterActor::paint
 * signal.</para></warning>
 *
 * Return value: a newly created Cairo context. Use cairo_destroy()
 *   to upload the contents of the context when done drawing
 *
 * Since: 1.0
 */
cairo_t *
clutter_cairo_texture_create_region (ClutterCairoTexture *self,
                                     gint                 x_offset,
                                     gint                 y_offset,
                                     gint                 width,
                                     gint                 height)
{
  ClutterCairoTexturePrivate *priv;
  ClutterCairoTextureContext *ctxt;
  ClutterCairoTextureRectangle region, area, inter;
  cairo_t *cr;

  g_return_val_if_fail (CLUTTER_IS_CAIRO_TEXTURE (self), NULL);

  clutter_warn_if_paint_fail (self);

  priv = self->priv;

  if (width < 0)
    width = priv->width;

  if (height < 0)
    height = priv->height;

  if (width == 0 || height == 0)
    {
      g_warning ("Unable to create a context for an image surface of "
                 "width %d and height %d. Set the surface size to be "
                 "at least 1 pixel by 1 pixel.",
                 width, height);
      return NULL;
    }

  if (!priv->cr_surface)
    return NULL;

  ctxt = g_new0 (ClutterCairoTextureContext, 1);
  ctxt->cairo = self;

  region.x = x_offset;
  region.y = y_offset;
  region.width = width;
  region.height = height;

  area.x = 0;
  area.y = 0;
  area.width = priv->width;
  area.height = priv->height;

  /* Limit the region to the visible rectangle */
  intersect_rectangles (&area, &region, &inter);

  ctxt->rect.x = inter.x;
  ctxt->rect.y = inter.y;
  ctxt->rect.width = inter.width;
  ctxt->rect.height = inter.height;

  cr = cairo_create (priv->cr_surface);
  cairo_set_user_data (cr, &clutter_cairo_texture_context_key,
		       ctxt, clutter_cairo_texture_context_destroy);

  return cr;
}

/**
 * clutter_cairo_texture_create:
 * @self: a #ClutterCairoTexture
 *
 * Creates a new Cairo context for the @cairo texture. It is
 * similar to using clutter_cairo_texture_create_region() with @x_offset
 * and @y_offset of 0, @width equal to the @cairo texture surface width
 * and @height equal to the @cairo texture surface height.
 *
 * <warning><para>Do not call this function within the paint virtual
 * function or from a callback to the #ClutterActor::paint
 * signal.</para></warning>
 *
 * Return value: a newly created Cairo context. Use cairo_destroy()
 *   to upload the contents of the context when done drawing
 *
 * Since: 1.0
 */
cairo_t *
clutter_cairo_texture_create (ClutterCairoTexture *self)
{
  g_return_val_if_fail (CLUTTER_IS_CAIRO_TEXTURE (self), NULL);

  clutter_warn_if_paint_fail (self);

  return clutter_cairo_texture_create_region (self, 0, 0, -1, -1);
}

/**
 * clutter_cairo_set_source_color:
 * @cr: a Cairo context
 * @color: a #ClutterColor
 *
 * Utility function for setting the source color of @cr using
 * a #ClutterColor.
 *
 * Since: 1.0
 */
void
clutter_cairo_set_source_color (cairo_t            *cr,
                                const ClutterColor *color)
{
  g_return_if_fail (cr != NULL);
  g_return_if_fail (color != NULL);

  if (color->alpha == 0xff)
    cairo_set_source_rgb (cr,
                          color->red / 255.0,
                          color->green / 255.0,
                          color->blue / 255.0);
  else
    cairo_set_source_rgba (cr,
                           color->red / 255.0,
                           color->green / 255.0,
                           color->blue / 255.0,
                           color->alpha / 255.0);
}

/**
 * clutter_cairo_texture_set_surface_size:
 * @self: a #ClutterCairoTexture
 * @width: the new width of the surface
 * @height: the new height of the surface
 *
 * Resizes the Cairo surface used by @self to @width and @height.
 *
 * Since: 1.0
 */
void
clutter_cairo_texture_set_surface_size (ClutterCairoTexture *self,
                                        guint                width,
                                        guint                height)
{
  ClutterCairoTexturePrivate *priv;

  g_return_if_fail (CLUTTER_IS_CAIRO_TEXTURE (self));

  priv = self->priv;

  if (width == priv->width && height == priv->height)
    return;

  g_object_freeze_notify (G_OBJECT (self));

  if (priv->width != width)
    {
      priv->width = width;
      g_object_notify (G_OBJECT (self), "surface-width");
    }

  if (priv->height != height)
    {
      priv->height = height;
      g_object_notify (G_OBJECT (self), "surface-height");
    }

  clutter_cairo_texture_surface_resize_internal (self);

  g_object_thaw_notify (G_OBJECT (self));
}

/**
 * clutter_cairo_texture_get_surface_size:
 * @self: a #ClutterCairoTexture
 * @width: return location for the surface width, or %NULL
 * @height: return location for the surface height, or %NULL
 *
 * Retrieves the surface width and height for @self.
 *
 * Since: 1.0
 */
void
clutter_cairo_texture_get_surface_size (ClutterCairoTexture *self,
                                        guint               *width,
                                        guint               *height)
{
  g_return_if_fail (CLUTTER_IS_CAIRO_TEXTURE (self));

  if (width)
    *width = self->priv->width;

  if (height)
    *height = self->priv->height;
}

/**
 * clutter_cairo_texture_clear:
 * @self: a #ClutterCairoTexture
 *
 * Clears @self's internal drawing surface, so that the next upload
 * will replace the previous contents of the #ClutterCairoTexture
 * rather than adding to it.
 *
 * Since: 1.0
 */
void
clutter_cairo_texture_clear (ClutterCairoTexture *self)
{
  ClutterCairoTexturePrivate *priv;

  g_return_if_fail (CLUTTER_IS_CAIRO_TEXTURE (self));

  priv = self->priv;

  if (!priv->cr_surface_data)
    return;

  memset (priv->cr_surface_data, 0, priv->height * priv->rowstride);
}
