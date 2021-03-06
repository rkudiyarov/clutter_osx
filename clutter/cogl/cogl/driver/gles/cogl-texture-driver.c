/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2007,2008,2009 Intel Corporation.
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
 *
 * Authors:
 *  Matthew Allum  <mallum@openedhand.com>
 *  Neil Roberts   <neil@linux.intel.com>
 *  Robert Bragg   <robert@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl.h"
#include "cogl-internal.h"
#include "cogl-util.h"
#include "cogl-bitmap.h"
#include "cogl-bitmap-private.h"
#include "cogl-texture-private.h"
#include "cogl-material.h"
#include "cogl-material-opengl-private.h"
#include "cogl-context.h"
#include "cogl-handle.h"
#include "cogl-primitives.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "cogl-gles2-wrapper.h"

#define glTexImage3D ctx->drv.pf_glTexImage3D
#define glTexSubImage3D ctx->drv.pf_glTexSubImage3D

#ifndef GL_TEXTURE_3D
#define GL_TEXTURE_3D 0x806F
#endif
#ifndef GL_MAX_3D_TEXTURE_SIZE_OES
#define GL_MAX_3D_TEXTURE_SIZE_OES 0x8073
#endif

void
_cogl_texture_driver_gen (GLenum   gl_target,
                          GLsizei  n,
                          GLuint  *textures)
{
  unsigned int i;

  GE (glGenTextures (n, textures));

  for (i = 0; i < n; i++)
    {
      _cogl_bind_gl_texture_transient (gl_target, textures[i], FALSE);

      switch (gl_target)
        {
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
          /* GL_TEXTURE_MAG_FILTER defaults to GL_LINEAR, no need to set it */
          GE( glTexParameteri (gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR) );
          break;

        default:
          g_assert_not_reached();
        }
    }
}

void
_cogl_texture_driver_prep_gl_for_pixels_upload (int pixels_rowstride,
                                                int pixels_bpp)
{
  _cogl_texture_prep_gl_alignment_for_pixels_upload (pixels_rowstride);
}

void
_cogl_texture_driver_prep_gl_for_pixels_download (int pixels_rowstride,
                                                  int pixels_bpp)
{
  _cogl_texture_prep_gl_alignment_for_pixels_download (pixels_rowstride);
}

void
_cogl_texture_driver_upload_subregion_to_gl (GLenum       gl_target,
                                             GLuint       gl_handle,
                                             gboolean     is_foreign,
                                             int          src_x,
                                             int          src_y,
                                             int          dst_x,
                                             int          dst_y,
                                             int          width,
                                             int          height,
                                             CoglBitmap  *source_bmp,
				             GLuint       source_gl_format,
				             GLuint       source_gl_type)
{
  guint8 *data;
  CoglPixelFormat source_format = _cogl_bitmap_get_format (source_bmp);
  int bpp = _cogl_get_format_bpp (source_format);
  CoglBitmap *slice_bmp;
  int rowstride;

  /* NB: GLES doesn't support the GL_UNPACK_ROW_LENGTH,
   * GL_UNPACK_SKIP_PIXELS or GL_UNPACK_SKIP_ROWS pixel store options
   * so we can't directly source a sub-region from source_bmp, we need
   * to use a transient bitmap instead. */

  /* FIXME: optimize by not copying to intermediate slice bitmap when
   * source rowstride = bpp * width and the texture image is not
   * sliced */

  /* Setup temp bitmap for slice subregion */
  rowstride = bpp * width;
  slice_bmp = _cogl_bitmap_new_from_data (g_malloc (rowstride * height),
                                          source_format,
                                          width,
                                          height,
                                          rowstride,
                                          (CoglBitmapDestroyNotify)
                                          g_free,
                                          NULL);

  /* Setup gl alignment to match rowstride and top-left corner */
  _cogl_texture_driver_prep_gl_for_pixels_upload (rowstride, bpp);

  /* Copy subregion data */
  _cogl_bitmap_copy_subregion (source_bmp,
                               slice_bmp,
                               src_x,
                               src_y,
                               0, 0,
                               width, height);

  data = _cogl_bitmap_bind (slice_bmp, COGL_BUFFER_ACCESS_READ, 0);

  _cogl_bind_gl_texture_transient (gl_target, gl_handle, is_foreign);

  GE( glTexSubImage2D (gl_target, 0,
                       dst_x, dst_y,
                       width, height,
                       source_gl_format,
                       source_gl_type,
                       data) );

  _cogl_bitmap_unbind (slice_bmp);

  cogl_object_unref (slice_bmp);
}

void
_cogl_texture_driver_upload_to_gl (GLenum       gl_target,
                                   GLuint       gl_handle,
                                   gboolean     is_foreign,
                                   CoglBitmap  *source_bmp,
                                   GLint        internal_gl_format,
                                   GLuint       source_gl_format,
                                   GLuint       source_gl_type)
{
  int bpp = _cogl_get_format_bpp (_cogl_bitmap_get_format (source_bmp));
  int rowstride = _cogl_bitmap_get_rowstride (source_bmp);
  int bmp_width = _cogl_bitmap_get_width (source_bmp);
  int bmp_height = _cogl_bitmap_get_height (source_bmp);
  CoglBitmap *bmp;
  guint8 *data;

  /* If the rowstride can't be specified with just GL_ALIGNMENT alone
     then we need to copy the bitmap because there is no GL_ROW_LENGTH */
  if (rowstride / bpp != bmp_width)
    {
      bmp = _cogl_bitmap_new_from_data (g_malloc (rowstride * bmp_height),
                                        _cogl_bitmap_get_format (source_bmp),
                                        bmp_width,
                                        bmp_height,
                                        rowstride,
                                        (CoglBitmapDestroyNotify) g_free,
                                        NULL);

      _cogl_bitmap_copy_subregion (source_bmp,
                                   bmp,
                                   0, 0, 0, 0,
                                   bmp_width,
                                   bmp_height);
    }
  else
    bmp = cogl_object_ref (source_bmp);

  /* Setup gl alignment to match rowstride and top-left corner */
  _cogl_texture_driver_prep_gl_for_pixels_upload (rowstride, bpp);

  _cogl_bind_gl_texture_transient (gl_target, gl_handle, is_foreign);

  data = _cogl_bitmap_bind (bmp, COGL_BUFFER_ACCESS_READ, 0);

  GE( glTexImage2D (gl_target, 0,
                    internal_gl_format,
                    bmp_width, bmp_height,
                    0,
                    source_gl_format,
                    source_gl_type,
                    data) );

  _cogl_bitmap_unbind (bmp);

  cogl_object_unref (bmp);
}

void
_cogl_texture_driver_upload_to_gl_3d (GLenum       gl_target,
                                      GLuint       gl_handle,
                                      gboolean     is_foreign,
                                      GLint        height,
                                      GLint        depth,
                                      CoglBitmap  *source_bmp,
                                      GLint        internal_gl_format,
                                      GLuint       source_gl_format,
                                      GLuint       source_gl_type)
{
  int bpp = _cogl_get_format_bpp (_cogl_bitmap_get_format (source_bmp));
  int rowstride = _cogl_bitmap_get_rowstride (source_bmp);
  int bmp_width = _cogl_bitmap_get_width (source_bmp);
  int bmp_height = _cogl_bitmap_get_height (source_bmp);
  guint8 *data;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  _cogl_bind_gl_texture_transient (gl_target, gl_handle, is_foreign);

  /* If the rowstride or image height can't be specified with just
     GL_ALIGNMENT alone then we need to copy the bitmap because there
     is no GL_ROW_LENGTH */
  if (rowstride / bpp != bmp_width ||
      height != bmp_height / depth)
    {
      CoglBitmap *bmp;
      int image_height = bmp_height / depth;
      int i;

      _cogl_texture_driver_prep_gl_for_pixels_upload (bmp_width * bpp, bpp);

      /* Initialize the texture with empty data and then upload each
         image with a sub-region update */

      GE( glTexImage3D (gl_target,
                        0, /* level */
                        internal_gl_format,
                        bmp_width,
                        height,
                        depth,
                        0,
                        source_gl_format,
                        source_gl_type,
                        NULL) );

      bmp = _cogl_bitmap_new_from_data (g_malloc (bpp * bmp_width * height),
                                        _cogl_bitmap_get_format (source_bmp),
                                        bmp_width,
                                        height,
                                        bpp * bmp_width,
                                        (CoglBitmapDestroyNotify) g_free,
                                        NULL);

      for (i = 0; i < depth; i++)
        {
          _cogl_bitmap_copy_subregion (source_bmp,
                                       bmp,
                                       0, image_height * i,
                                       0, 0,
                                       bmp_width,
                                       bmp_height);

          data = _cogl_bitmap_bind (bmp,
                                    COGL_BUFFER_ACCESS_READ, 0);

          GE( glTexSubImage3D (gl_target,
                               0, /* level */
                               0, /* xoffset */
                               0, /* yoffset */
                               i, /* zoffset */
                               bmp_width, /* width */
                               height, /* height */
                               1, /* depth */
                               source_gl_format,
                               source_gl_type,
                               data) );

          _cogl_bitmap_unbind (bmp);
        }

      cogl_object_unref (bmp);
    }
  else
    {
      data = _cogl_bitmap_bind (source_bmp, COGL_BUFFER_ACCESS_READ, 0);

      _cogl_texture_driver_prep_gl_for_pixels_upload (rowstride, bpp);

      GE( glTexImage3D (gl_target,
                        0, /* level */
                        internal_gl_format,
                        bmp_width,
                        height,
                        depth,
                        0,
                        source_gl_format,
                        source_gl_type,
                        data) );

      _cogl_bitmap_unbind (source_bmp);
    }
}

/* NB: GLES doesn't support glGetTexImage2D, so cogl-texture will instead
 * fallback to a generic render + readpixels approach to downloading
 * texture data. (See _cogl_texture_draw_and_read() ) */
gboolean
_cogl_texture_driver_gl_get_tex_image (GLenum  gl_target,
                                       GLenum  dest_gl_format,
                                       GLenum  dest_gl_type,
                                       guint8 *dest)
{
  return FALSE;
}

gboolean
_cogl_texture_driver_size_supported_3d (GLenum gl_target,
                                        GLenum gl_format,
                                        GLenum gl_type,
                                        int    width,
                                        int    height,
                                        int    depth)
{
  GLint max_size;

  /* GLES doesn't support a proxy texture target so let's at least
     check whether the size is greater than
     GL_MAX_3D_TEXTURE_SIZE_OES */
  GE( glGetIntegerv (GL_MAX_3D_TEXTURE_SIZE_OES, &max_size) );

  return width <= max_size && height <= max_size && depth <= max_size;
}

gboolean
_cogl_texture_driver_size_supported (GLenum gl_target,
                                     GLenum gl_format,
                                     GLenum gl_type,
                                     int    width,
                                     int    height)
{
  GLint max_size;

  /* GLES doesn't support a proxy texture target so let's at least
     check whether the size is greater than GL_MAX_TEXTURE_SIZE */
  GE( glGetIntegerv (GL_MAX_TEXTURE_SIZE, &max_size) );

  return width <= max_size && height <= max_size;
}

void
_cogl_texture_driver_try_setting_gl_border_color (
                                              GLuint   gl_target,
                                              const GLfloat *transparent_color)
{
  /* FAIL! */
}

gboolean
_cogl_pixel_format_from_gl_internal (GLenum            gl_int_format,
				     CoglPixelFormat  *out_format)
{
  return TRUE;
}

CoglPixelFormat
_cogl_pixel_format_to_gl (CoglPixelFormat  format,
			  GLenum          *out_glintformat,
			  GLenum          *out_glformat,
			  GLenum          *out_gltype)
{
  CoglPixelFormat required_format;
  GLenum          glintformat = 0;
  GLenum          glformat = 0;
  GLenum          gltype = 0;

  /* FIXME: check YUV support */

  required_format = format;

  /* Find GL equivalents */
  switch (format & COGL_UNPREMULT_MASK)
    {
    case COGL_PIXEL_FORMAT_A_8:
      glintformat = GL_ALPHA;
      glformat = GL_ALPHA;
      gltype = GL_UNSIGNED_BYTE;
      break;
    case COGL_PIXEL_FORMAT_G_8:
      glintformat = GL_LUMINANCE;
      glformat = GL_LUMINANCE;
      gltype = GL_UNSIGNED_BYTE;
      break;

      /* Just one 24-bit ordering supported */
    case COGL_PIXEL_FORMAT_RGB_888:
    case COGL_PIXEL_FORMAT_BGR_888:
      glintformat = GL_RGB;
      glformat = GL_RGB;
      gltype = GL_UNSIGNED_BYTE;
      required_format = COGL_PIXEL_FORMAT_RGB_888;
      break;

      /* Just one 32-bit ordering supported */
    case COGL_PIXEL_FORMAT_RGBA_8888:
    case COGL_PIXEL_FORMAT_BGRA_8888:
    case COGL_PIXEL_FORMAT_ARGB_8888:
    case COGL_PIXEL_FORMAT_ABGR_8888:
      glintformat = GL_RGBA;
      glformat = GL_RGBA;
      gltype = GL_UNSIGNED_BYTE;
      required_format = COGL_PIXEL_FORMAT_RGBA_8888;
      required_format |= (format & COGL_PREMULT_BIT);
      break;

      /* The following three types of channel ordering
       * are always defined using system word byte
       * ordering (even according to GLES spec) */
    case COGL_PIXEL_FORMAT_RGB_565:
      glintformat = GL_RGB;
      glformat = GL_RGB;
      gltype = GL_UNSIGNED_SHORT_5_6_5;
      break;
    case COGL_PIXEL_FORMAT_RGBA_4444:
      glintformat = GL_RGBA;
      glformat = GL_RGBA;
      gltype = GL_UNSIGNED_SHORT_4_4_4_4;
      break;
    case COGL_PIXEL_FORMAT_RGBA_5551:
      glintformat = GL_RGBA;
      glformat = GL_RGBA;
      gltype = GL_UNSIGNED_SHORT_5_5_5_1;
      break;

      /* FIXME: check extensions for YUV support */
    default:
      break;
    }

  if (out_glintformat != NULL)
    *out_glintformat = glintformat;
  if (out_glformat != NULL)
    *out_glformat = glformat;
  if (out_gltype != NULL)
    *out_gltype = gltype;

  return required_format;
}

gboolean
_cogl_texture_driver_allows_foreign_gl_target (GLenum gl_target)
{
  /* Allow 2-dimensional textures only */
  if (gl_target != GL_TEXTURE_2D)
    return FALSE;
  return TRUE;
}

void
_cogl_texture_driver_gl_generate_mipmaps (GLenum gl_target)
{
  GE( _cogl_wrap_glGenerateMipmap (gl_target) );
}

CoglPixelFormat
_cogl_texture_driver_find_best_gl_get_data_format (
                                             CoglPixelFormat  format,
                                             GLenum          *closest_gl_format,
                                             GLenum          *closest_gl_type)
{
  /* Find closest format that's supported by GL
     (Can't use _cogl_pixel_format_to_gl since available formats
      when reading pixels on GLES are severely limited) */
  *closest_gl_format = GL_RGBA;
  *closest_gl_type = GL_UNSIGNED_BYTE;
  return COGL_PIXEL_FORMAT_RGBA_8888;
}

