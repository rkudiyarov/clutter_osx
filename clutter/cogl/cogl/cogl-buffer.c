/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2010 Intel Corporation.
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
 *   Damien Lespiau <damien.lespiau@intel.com>
 *   Robert Bragg <robert@linux.intel.com>
 */

/* For an overview of the functionality implemented here, please see
 * cogl-buffer.h, which contains the gtk-doc section overview for the
 * Pixel Buffers API.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "cogl.h"
#include "cogl-internal.h"
#include "cogl-util.h"
#include "cogl-context.h"
#include "cogl-handle.h"
#include "cogl-pixel-array-private.h"

/*
 * GL/GLES compatibility defines for the buffer API:
 */

#if defined (HAVE_COGL_GL)

#define glGenBuffers ctx->drv.pf_glGenBuffers
#define glBindBuffer ctx->drv.pf_glBindBuffer
#define glBufferData ctx->drv.pf_glBufferData
#define glBufferSubData ctx->drv.pf_glBufferSubData
#define glGetBufferSubData ctx->drv.pf_glGetBufferSubData
#define glDeleteBuffers ctx->drv.pf_glDeleteBuffers
#define glMapBuffer ctx->drv.pf_glMapBuffer
#define glUnmapBuffer ctx->drv.pf_glUnmapBuffer

#elif defined (HAVE_COGL_GLES2)

#include "../gles/cogl-gles2-wrapper.h"

#endif

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8893
#endif

/* XXX:
 * The CoglHandle macros don't support any form of inheritance, so for
 * now we implement the CoglObject support for the CoglBuffer
 * abstract class manually.
 */

void
_cogl_buffer_register_buffer_type (GQuark type)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  ctx->buffer_types = g_slist_prepend (ctx->buffer_types,
                                       GINT_TO_POINTER (type));
}

gboolean
cogl_is_buffer (const void *object)
{
  const CoglHandleObject *obj = object;
  GSList *l;

  _COGL_GET_CONTEXT (ctx, FALSE);

  if (object == NULL)
    return FALSE;

  for (l = ctx->buffer_types; l; l = l->next)
    if (GPOINTER_TO_INT (l->data) == obj->klass->type)
      return TRUE;

  return FALSE;
}

static GLenum
convert_bind_target_to_gl_target (CoglBufferBindTarget target)
{
  switch (target)
    {
      case COGL_BUFFER_BIND_TARGET_PIXEL_PACK:
        return GL_PIXEL_PACK_BUFFER;
      case COGL_BUFFER_BIND_TARGET_PIXEL_UNPACK:
        return GL_PIXEL_UNPACK_BUFFER;
      case COGL_BUFFER_BIND_TARGET_VERTEX_ARRAY:
        return GL_ARRAY_BUFFER;
      case COGL_BUFFER_BIND_TARGET_VERTEX_INDICES_ARRAY:
        return GL_ELEMENT_ARRAY_BUFFER;
      default:
        g_return_val_if_reached (COGL_BUFFER_BIND_TARGET_PIXEL_UNPACK);
    }
}

static guint8 *
bo_map (CoglBuffer       *buffer,
        CoglBufferAccess  access,
        CoglBufferMapHint hints)
{
#ifndef COGL_HAS_GLES

  CoglPixelArray *pixel_array = COGL_PIXEL_ARRAY (buffer);
  guint8 *data;
  CoglBufferBindTarget target;
  GLenum gl_target;

  _COGL_GET_CONTEXT (ctx, NULL);

  target = buffer->last_target;
  _cogl_buffer_bind (buffer, target);

  gl_target = convert_bind_target_to_gl_target (target);

  /* create an empty store if we don't have one yet. creating the store
   * lazily allows the user of the CoglBuffer to set a hint before the
   * store is created. */
  if (!COGL_PIXEL_ARRAY_FLAG_IS_SET (pixel_array, STORE_CREATED) ||
      (hints & COGL_BUFFER_MAP_HINT_DISCARD))
    {
      GE( glBufferData (gl_target,
                        buffer->size,
                        NULL,
                        _cogl_buffer_hints_to_gl_enum (buffer->usage_hint,
                                                       buffer->update_hint)) );
      COGL_PIXEL_ARRAY_SET_FLAG (pixel_array, STORE_CREATED);
    }

  GE_RET( data, glMapBuffer (gl_target,
                             _cogl_buffer_access_to_gl_enum (access)) );
  if (data)
    COGL_BUFFER_SET_FLAG (buffer, MAPPED);

  _cogl_buffer_unbind (buffer);

  return data;

#else /* COGL_HAS_GLES */

  return NULL;

#endif /* COGL_HAS_GLES */
}

static void
bo_unmap (CoglBuffer *buffer)
{
#ifndef COGL_HAS_GLES
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  _cogl_buffer_bind (buffer, buffer->last_target);

  GE( glUnmapBuffer (convert_bind_target_to_gl_target (buffer->last_target)) );
  COGL_BUFFER_CLEAR_FLAG (buffer, MAPPED);

  _cogl_buffer_unbind (buffer);
#else
  g_return_if_reached ();
#endif
}

static gboolean
bo_set_data (CoglBuffer   *buffer,
             unsigned int  offset,
             const guint8 *data,
             unsigned int  size)
{
  CoglPixelArray *pixel_array = COGL_PIXEL_ARRAY (buffer);
  CoglBufferBindTarget target;
  GLenum gl_target;

  _COGL_GET_CONTEXT (ctx, FALSE);

  target = buffer->last_target;
  _cogl_buffer_bind (buffer, target);

  gl_target = convert_bind_target_to_gl_target (target);

  /* create an empty store if we don't have one yet. creating the store
   * lazily allows the user of the CoglBuffer to set a hint before the
   * store is created. */
  if (!COGL_PIXEL_ARRAY_FLAG_IS_SET (pixel_array, STORE_CREATED))
    {
      GE( glBufferData (gl_target,
                        buffer->size,
                        NULL,
                        _cogl_buffer_hints_to_gl_enum (buffer->usage_hint,
                                                       buffer->update_hint)) );
      COGL_PIXEL_ARRAY_SET_FLAG (pixel_array, STORE_CREATED);
    }

  GE( glBufferSubData (gl_target, offset, size, data) );

  _cogl_buffer_unbind (buffer);

  return TRUE;
}

/*
 * Fallback path, buffer->data points to a malloc'ed buffer.
 */

static guint8 *
malloc_map (CoglBuffer       *buffer,
            CoglBufferAccess  access,
            CoglBufferMapHint hints)
{
  COGL_BUFFER_SET_FLAG (buffer, MAPPED);
  return buffer->data;
}

static void
malloc_unmap (CoglBuffer *buffer)
{
  COGL_BUFFER_CLEAR_FLAG (buffer, MAPPED);
}

static gboolean
malloc_set_data (CoglBuffer   *buffer,
                 unsigned int  offset,
                 const guint8 *data,
                 unsigned int  size)
{
  memcpy (buffer->data + offset, data, size);
  return TRUE;
}

void
_cogl_buffer_initialize (CoglBuffer           *buffer,
                         unsigned int          size,
                         gboolean              use_malloc,
                         CoglBufferBindTarget  default_target,
                         CoglBufferUsageHint   usage_hint,
                         CoglBufferUpdateHint  update_hint)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  buffer->flags       = COGL_BUFFER_FLAG_NONE;
  buffer->size        = size;
  buffer->last_target = default_target;
  buffer->usage_hint  = usage_hint;
  buffer->update_hint = update_hint;
  buffer->data        = NULL;

  if (use_malloc)
    {
      buffer->vtable.map = malloc_map;
      buffer->vtable.unmap = malloc_unmap;
      buffer->vtable.set_data = malloc_set_data;

      buffer->data = g_malloc (size);
    }
  else
    {
      buffer->vtable.map = bo_map;
      buffer->vtable.unmap = bo_unmap;
      buffer->vtable.set_data = bo_set_data;

      GE( glGenBuffers (1, &buffer->gl_handle) );
      COGL_BUFFER_SET_FLAG (buffer, BUFFER_OBJECT);
    }
}

void
_cogl_buffer_fini (CoglBuffer *buffer)
{
  g_return_if_fail (!COGL_BUFFER_FLAG_IS_SET (buffer, MAPPED));
}

/* OpenGL ES 1.1 and 2 have a GL_OES_mapbuffer extension that is able to map
 * VBOs for write only, we don't support that in CoglBuffer */
#if defined (COGL_HAS_GLES)
GLenum
_cogl_buffer_access_to_gl_enum (CoglBufferAccess access)
{
  return 0;
}

#else
GLenum
_cogl_buffer_access_to_gl_enum (CoglBufferAccess access)
{
  if ((access & COGL_BUFFER_ACCESS_READ_WRITE) == COGL_BUFFER_ACCESS_READ_WRITE)
    return GL_READ_WRITE;
  else if (access & COGL_BUFFER_ACCESS_WRITE)
    return GL_WRITE_ONLY;
  else
    return GL_READ_ONLY;
}
#endif

/* OpenGL ES 1.1 and 2 only know about STATIC_DRAW and DYNAMIC_DRAW */
#if defined (COGL_HAS_GLES)
GLenum
_cogl_buffer_hints_to_gl_enum (CoglBufferUsageHint usage_hint,
                               CoglBufferUpdateHint update_hint)
{
  /* usage hint is always TEXTURE for now */
  if (update_hint == COGL_BUFFER_UPDATE_HINT_STATIC)
      return GL_STATIC_DRAW;
  return GL_DYNAMIC_DRAW;
}
#else
GLenum
_cogl_buffer_hints_to_gl_enum (CoglBufferUsageHint  usage_hint,
                               CoglBufferUpdateHint update_hint)
{
  /* usage hint is always TEXTURE for now */
  if (update_hint == COGL_BUFFER_UPDATE_HINT_STATIC)
    return GL_STATIC_DRAW;
  if (update_hint == COGL_BUFFER_UPDATE_HINT_DYNAMIC)
    return GL_DYNAMIC_DRAW;
  if (update_hint == COGL_BUFFER_UPDATE_HINT_STREAM)
    return GL_STREAM_DRAW;

  return GL_STATIC_DRAW;
}
#endif

void
_cogl_buffer_bind (CoglBuffer *buffer, CoglBufferBindTarget target)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  g_return_if_fail (buffer != NULL);

  /* Don't allow binding the buffer to multiple targets at the same time */
  g_return_if_fail (ctx->current_buffer[buffer->last_target] != buffer);

  /* Don't allow nesting binds to the same target */
  g_return_if_fail (ctx->current_buffer[target] == NULL);

  buffer->last_target = target;

  if (COGL_BUFFER_FLAG_IS_SET (buffer, BUFFER_OBJECT))
    {
      GLenum gl_target = convert_bind_target_to_gl_target (buffer->last_target);
      GE( glBindBuffer (gl_target, buffer->gl_handle) );
    }

  ctx->current_buffer[target] = buffer;
}

void
_cogl_buffer_unbind (CoglBuffer *buffer)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  g_return_if_fail (buffer != NULL);

  /* the unbind should pair up with a previous bind */
  g_return_if_fail (ctx->current_buffer[buffer->last_target] == buffer);

  if (COGL_BUFFER_FLAG_IS_SET (buffer, BUFFER_OBJECT))
    {
      GLenum gl_target = convert_bind_target_to_gl_target (buffer->last_target);
      GE( glBindBuffer (gl_target, 0) );
    }

  ctx->current_buffer[buffer->last_target] = NULL;
}

unsigned int
cogl_buffer_get_size (CoglBuffer *buffer)
{
  if (!cogl_is_buffer (buffer))
    return 0;

  return COGL_BUFFER (buffer)->size;
}

void
cogl_buffer_set_update_hint (CoglBuffer *buffer,
                             CoglBufferUpdateHint hint)
{
  if (!cogl_is_buffer (buffer))
    return;

  if (G_UNLIKELY (hint > COGL_BUFFER_UPDATE_HINT_STREAM))
    hint = COGL_BUFFER_UPDATE_HINT_STATIC;

  buffer->update_hint = hint;
}

CoglBufferUpdateHint
cogl_buffer_get_update_hint (CoglBuffer *buffer)
{
  if (!cogl_is_buffer (buffer))
    return FALSE;

  return buffer->update_hint;
}

guint8 *
cogl_buffer_map (CoglBuffer        *buffer,
                 CoglBufferAccess   access,
                 CoglBufferMapHint  hints)
{
  if (!cogl_is_buffer (buffer))
    return NULL;

  if (COGL_BUFFER_FLAG_IS_SET (buffer, MAPPED))
    return buffer->data;

  buffer->data = buffer->vtable.map (buffer, access, hints);
  return buffer->data;
}

void
cogl_buffer_unmap (CoglBuffer *buffer)
{
  if (!cogl_is_buffer (buffer))
    return;

  if (!COGL_BUFFER_FLAG_IS_SET (buffer, MAPPED))
    return;

  buffer->vtable.unmap (buffer);
}

gboolean
cogl_buffer_set_data (CoglBuffer   *buffer,
                      gsize         offset,
                      const guint8 *data,
                      gsize         size)
{
  if (!cogl_is_buffer (buffer))
    return FALSE;

  if (G_UNLIKELY((offset + size) > buffer->size))
    return FALSE;

  return buffer->vtable.set_data (buffer, offset, data, size);
}
