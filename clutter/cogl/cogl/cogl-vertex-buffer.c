/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2008,2009 Intel Corporation.
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
 *   Robert Bragg <robert@linux.intel.com>
 */

/* XXX: For an overview of the functionality implemented here, please
 * see cogl-vertex-buffer.h, which contains the gtk-doc section overview
 * for the Vertex Buffers API.
 */

/*
 * TODO: We need to do a better job of minimizing when we call glVertexPointer
 * and pals in enable_state_for_drawing_buffer
 *
 * We should have an internal 2-tuple cache of (VBO, offset) for each of them
 * so we can avoid some GL calls. We could have cogl wrappers for the
 * gl*Pointer funcs that look like this:
 *
 * cogl_vertex_pointer (n_components, gl_type, stride, vbo, offset);
 * cogl_color_pointer (n_components, gl_type, stride, vbo, offset);
 *
 * They would also accept NULL for the VBO handle to support old style vertex
 * arrays.
 *
 * TODO:
 * Actually hook this up to the cogl shaders infrastructure. The vertex
 * buffer API has been designed to allow adding of arbitrary attributes for use
 * with shaders, but this has yet to be actually plumbed together and tested.
 * The bits we are missing:
 * - cogl_program_use doesn't currently record within ctx-> which program
 *   is currently in use so a.t.m only Clutter knows the current shader.
 * - We don't query the current shader program for the generic vertex indices
 *   (using glGetAttribLocation) so that we can call glEnableVertexAttribArray
 *   with those indices.
 *   (currently we just make up consecutive indices)
 * - some dirty flag mechanims to know when the shader program has changed
 *   so we don't need to re-query it each time we draw a buffer.
 *
 * TODO:
 * There is currently no API for querying back info about a buffer, E.g.:
 * cogl_vertex_buffer_get_n_vertices (buffer_handle);
 * cogl_vertex_buffer_get_n_components (buffer_handle, "attrib_name");
 * cogl_vertex_buffer_get_stride (buffer_handle, "attrib_name");
 * cogl_vertex_buffer_get_normalized (buffer_handle, "attrib_name");
 * cogl_vertex_buffer_map (buffer_handle, "attrib_name");
 * cogl_vertex_buffer_unmap (buffer_handle, "attrib_name");
 * (Realistically I wouldn't expect anyone to use such an API to examine the
 *  contents of a buffer for modification, since you'd need to handle too many
 *  possibilities, but never the less there might be other value in these.)

 * TODO:
 * It may be worth exposing the underlying VBOs for some advanced use
 * cases, e.g.:
 * handle = cogl_vbo_new (COGL_VBO_FLAG_STATIC);
 * pointer = cogl_vbo_map (handle, COGL_VBO_FLAG_WRITEONLY);
 * cogl_vbo_unmap (handle);
 * cogl_vbo_set_data (handle, size, data);
 * cogl_vbo_set_sub_data (handle, offset, size, data);
 * cogl_vbo_set_usage_hint (COGL_VBO_FLAG_DYNAMIC);
 *
 * TODO:
 * Experiment with wider use of the vertex buffers API internally to Cogl.
 * - There is potential, I think, for this API to become a work-horse API
 *   within COGL for submitting geometry to the GPU, and could unify some of
 *   the GL/GLES code paths.
 * E.g.:
 * - Try creating a per-context vertex buffer cache for cogl_texture_rectangle
 *   to sit on top of.
 * - Try saving the tesselation of paths/polygons into vertex buffers
 *   internally.
 *
 * TODO
 * Expose API that lets developers get back a buffer handle for a particular
 * polygon so they may add custom attributes to them.
 * - It should be possible to query/modify attributes efficiently, in place,
 *   avoiding copies. It would not be acceptable to simply require that
 *   developers must query back the n_vertices of a buffer and then the
 *   n_components, type and stride etc of each attribute since there
 *   would be too many combinations to realistically handle.
 *
 * - In practice, some cases might be best solved with a higher level
 *   EditableMesh API, (see futher below) but for many cases I think an
 *   API like this might be appropriate:
 *
 * cogl_vertex_buffer_foreach_vertex (buffer_handle,
 *                                    (AttributesBufferIteratorFunc)callback,
 *			              "gl_Vertex", "gl_Color", NULL);
 * static void callback (CoglVertexBufferVertex *vert)
 * {
 *    GLfloat *pos = vert->attrib[0];
 *    GLubyte *color = vert->attrib[1];
 *    GLfloat *new_attrib = buf[vert->index];
 *
 *    new_attrib = pos*color;
 * }
 *
 * TODO
 * Think about a higher level Mesh API for building/modifying attribute buffers
 * - E.g. look at Blender for inspiration here. They can build a mesh from
 *   "MVert", "MFace" and "MEdge" primitives.
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
#include "cogl-vertex-buffer-private.h"
#include "cogl-texture-private.h"
#include "cogl-material-private.h"
#include "cogl-material-opengl-private.h"
#include "cogl-primitives.h"
#include "cogl-framebuffer-private.h"
#include "cogl-journal-private.h"

#define PAD_FOR_ALIGNMENT(VAR, TYPE_SIZE) \
  (VAR = TYPE_SIZE + ((VAR - 1) & ~(TYPE_SIZE - 1)))


/*
 * GL/GLES compatability defines for VBO thingies:
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
#define glClientActiveTexture ctx->drv.pf_glClientActiveTexture
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER GL_ARRAY_BUFFER_ARB
#endif

#elif defined (HAVE_COGL_GLES2)

#include "../gles/cogl-gles2-wrapper.h"

#endif

/* This isn't defined in the GLES headers */
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif

/*
 * GL/GLES compatability defines for shader things:
 */

#if defined (HAVE_COGL_GL)

#define glVertexAttribPointer ctx->drv.pf_glVertexAttribPointer
#define glEnableVertexAttribArray ctx->drv.pf_glEnableVertexAttribArray
#define glDisableVertexAttribArray ctx->drv.pf_glDisableVertexAttribArray
#define MAY_HAVE_PROGRAMABLE_GL

#elif defined (HAVE_COGL_GLES2)

/* NB: GLES2 had shaders in core since day one so again we don't need
 * defines in this case: */
#define MAY_HAVE_PROGRAMABLE_GL

#endif

#ifndef HAVE_COGL_GL

/* GLES doesn't have glDrawRangeElements, so we simply pretend it does
 * but that it makes no use of the start, end constraints: */
#define glDrawRangeElements(mode, start, end, count, type, indices) \
  glDrawElements (mode, count, type, indices)

#else /* HAVE_COGL_GL */

#define glDrawRangeElements(mode, start, end, count, type, indices) \
  ctx->drv.pf_glDrawRangeElements (mode, start, end, count, type, indices)

#endif /* HAVE_COGL_GL */

static void _cogl_vertex_buffer_free (CoglVertexBuffer *buffer);
static void _cogl_vertex_buffer_indices_free (CoglVertexBufferIndices *buffer_indices);

COGL_HANDLE_DEFINE (VertexBuffer, vertex_buffer);
COGL_OBJECT_DEFINE_DEPRECATED_REF_COUNTING (vertex_buffer);
COGL_HANDLE_DEFINE (VertexBufferIndices, vertex_buffer_indices);

CoglHandle
cogl_vertex_buffer_new (unsigned int n_vertices)
{
  CoglVertexBuffer *buffer = g_slice_alloc (sizeof (CoglVertexBuffer));

  buffer->n_vertices = n_vertices;

  buffer->submitted_vbos = NULL;
  buffer->new_attributes = NULL;

  /* return COGL_INVALID_HANDLE; */
  return _cogl_vertex_buffer_handle_new (buffer);
}

unsigned int
cogl_vertex_buffer_get_n_vertices (CoglHandle handle)
{
  CoglVertexBuffer *buffer;

  if (!cogl_is_vertex_buffer (handle))
    return 0;

  buffer = _cogl_vertex_buffer_pointer_from_handle (handle);

  return buffer->n_vertices;
}

/* There are a number of standard OpenGL attributes that we deal with
 * specially. These attributes are all namespaced with a "gl_" prefix
 * so we should catch any typos instead of silently adding a custom
 * attribute.
 */
static CoglVertexBufferAttribFlags
validate_gl_attribute (const char *gl_attribute,
		       guint8 n_components,
		       guint8 *texture_unit)
{
  CoglVertexBufferAttribFlags type;
  char *detail_seperator = NULL;
  int name_len;

  detail_seperator = strstr (gl_attribute, "::");
  if (detail_seperator)
    name_len = detail_seperator - gl_attribute;
  else
    name_len = strlen (gl_attribute);

  if (strncmp (gl_attribute, "Vertex", name_len) == 0)
    {
      if (G_UNLIKELY (n_components == 1))
        g_critical ("glVertexPointer doesn't allow 1 component vertex "
                    "positions so we currently only support \"gl_Vertex\" "
                    "attributes where n_components == 2, 3 or 4");
      type = COGL_VERTEX_BUFFER_ATTRIB_FLAG_VERTEX_ARRAY;
    }
  else if (strncmp (gl_attribute, "Color", name_len) == 0)
    {
      if (G_UNLIKELY (n_components != 3 && n_components != 4))
        g_critical ("glColorPointer expects 3 or 4 component colors so we "
                    "currently only support \"gl_Color\" attributes where "
                    "n_components == 3 or 4");
      type = COGL_VERTEX_BUFFER_ATTRIB_FLAG_COLOR_ARRAY;
    }
  else if (strncmp (gl_attribute,
		    "MultiTexCoord",
		    strlen ("MultiTexCoord")) == 0)
    {
      unsigned int unit;

      if (sscanf (gl_attribute, "MultiTexCoord%u", &unit) != 1)
	{
	  g_warning ("gl_MultiTexCoord attributes should include a\n"
		     "texture unit number, E.g. gl_MultiTexCoord0\n");
	  unit = 0;
	}
      /* FIXME: validate any '::' delimiter for this case */
      *texture_unit = unit;
      type = COGL_VERTEX_BUFFER_ATTRIB_FLAG_TEXTURE_COORD_ARRAY;
    }
  else if (strncmp (gl_attribute, "Normal", name_len) == 0)
    {
      if (G_UNLIKELY (n_components != 3))
        g_critical ("glNormalPointer expects 3 component normals so we "
                    "currently only support \"gl_Normal\" attributes where "
                    "n_components == 3");
      type = COGL_VERTEX_BUFFER_ATTRIB_FLAG_NORMAL_ARRAY;
    }
  else
    {
      g_warning ("Unknown gl_* attribute name gl_%s\n", gl_attribute);
      type = COGL_VERTEX_BUFFER_ATTRIB_FLAG_INVALID;
    }

  return type;
}

/* This validates that a custom attribute name is a valid GLSL variable name
 *
 * NB: attribute names may have a detail component delimited using '::' E.g.
 * custom_attrib::foo or custom_attrib::bar
 *
 * maybe I should hang a compiled regex somewhere to handle this
 */
static gboolean
validate_custom_attribute_name (const char *attribute_name)
{
  char *detail_seperator = NULL;
  int name_len;
  int i;

  detail_seperator = strstr (attribute_name, "::");
  if (detail_seperator)
    name_len = detail_seperator - attribute_name;
  else
    name_len = strlen (attribute_name);

  if (name_len == 0
      || !g_ascii_isalpha (attribute_name[0])
      || attribute_name[0] != '_')
    return FALSE;

  for (i = 1; i < name_len; i++)
    if (!g_ascii_isalnum (attribute_name[i]) || attribute_name[i] != '_')
      return FALSE;

  return TRUE;
}

/* Iterates the CoglVertexBufferVBOs of a buffer and creates a flat list
 * of all the submitted attributes
 *
 * Note: The CoglVertexBufferAttrib structs are deep copied.
 */
static GList *
copy_submitted_attributes_list (CoglVertexBuffer *buffer)
{
  GList *tmp;
  GList *submitted_attributes = NULL;

  for (tmp = buffer->submitted_vbos; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferVBO *cogl_vbo = tmp->data;
      GList *tmp2;

      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *attribute = tmp2->data;
	  CoglVertexBufferAttrib *copy =
            g_slice_alloc (sizeof (CoglVertexBufferAttrib));
	  *copy = *attribute;
	  submitted_attributes = g_list_prepend (submitted_attributes, copy);
	}
    }
  return submitted_attributes;
}

static CoglVertexBufferAttribFlags
get_attribute_gl_type_flag_from_gl_type (GLenum gl_type)
{
  switch (gl_type)
  {
    case GL_BYTE:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_BYTE;
    case GL_UNSIGNED_BYTE:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_BYTE;
    case GL_SHORT:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_SHORT;
    case GL_UNSIGNED_SHORT:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_SHORT;
    case GL_FLOAT:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_FLOAT;
#if HAVE_COGL_GL
    case GL_INT:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_INT;
    case GL_UNSIGNED_INT:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_INT;
    case GL_DOUBLE:
      return COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_DOUBLE;
#endif
    default:
      g_warning ("Attribute Buffers API: "
                 "Unrecognised OpenGL type enum 0x%08x\n", gl_type);
      return 0;
  }
}

static gsize
get_gl_type_size (CoglVertexBufferAttribFlags flags)
{
  CoglVertexBufferAttribFlags gl_type =
    flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_MASK;

  switch (gl_type)
    {
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_BYTE:
      return sizeof (GLbyte);
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_BYTE:
      return sizeof (GLubyte);
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_SHORT:
      return sizeof (GLshort);
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_SHORT:
      return sizeof (GLushort);
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_FLOAT:
      return sizeof (GLfloat);
#if HAVE_COGL_GL
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_INT:
      return sizeof (GLint);
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_INT:
      return sizeof (GLuint);
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_DOUBLE:
      return sizeof (GLdouble);
#endif
    default:
      g_warning ("Vertex Buffer API: Unrecognised OpenGL type enum 0x%08x\n", gl_type);
      return 0;
    }
}

void
cogl_vertex_buffer_add (CoglHandle         handle,
		        const char        *attribute_name,
			guint8             n_components,
			CoglAttributeType  type,
			gboolean           normalized,
			guint16            stride,
			const void        *pointer)
{
  CoglVertexBuffer *buffer;
  GQuark name_quark = g_quark_from_string (attribute_name);
  gboolean modifying_an_attrib = FALSE;
  CoglVertexBufferAttrib *attribute;
  CoglVertexBufferAttribFlags flags = 0;
  guint8 texture_unit = 0;
  GList *tmp;

  if (!cogl_is_vertex_buffer (handle))
    return;

  buffer = _cogl_vertex_buffer_pointer_from_handle (handle);

  /* The submit function works by diffing between submitted_attributes
   * and new_attributes to minimize the upload bandwidth + cost of
   * allocating new VBOs, so if there isn't already a list of new_attributes
   * we create one: */
  if (!buffer->new_attributes)
    buffer->new_attributes = copy_submitted_attributes_list (buffer);

  /* Note: we first look for an existing attribute that we are modifying
   * so we may skip needing to validate the name */
  for (tmp = buffer->new_attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *submitted_attribute = tmp->data;
      if (submitted_attribute->name == name_quark)
	{
	  modifying_an_attrib = TRUE;

	  attribute = submitted_attribute;

	  /* since we will skip validate_gl_attribute in this case, we need
	   * to pluck out the attribute type before overwriting the flags: */
	  flags |=
            attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_TYPE_MASK;
	  break;
	}
    }

  if (!modifying_an_attrib)
    {
      /* Validate the attribute name, is suitable as a variable name */
      if (strncmp (attribute_name, "gl_", 3) == 0)
	{
	  flags |= validate_gl_attribute (attribute_name + 3,
					  n_components,
					  &texture_unit);
	  if (flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_INVALID)
	    return;
	}
      else
	{
	  flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_CUSTOM_ARRAY;
	  if (validate_custom_attribute_name (attribute_name))
	    return;
	}

      attribute = g_slice_alloc (sizeof (CoglVertexBufferAttrib));
    }

  attribute->name = g_quark_from_string (attribute_name);
  attribute->n_components = n_components;
  attribute->stride = buffer->n_vertices > 1 ? stride : 0;
  attribute->u.pointer = pointer;
  attribute->texture_unit = texture_unit;

  flags |= get_attribute_gl_type_flag_from_gl_type (type);
  flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_ENABLED;

  /* Note: We currently just assume, if an attribute is *ever* updated
   * then it should be taged as frequently changing. */
  if (modifying_an_attrib)
    flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_FREQUENT_RESUBMIT;
  else
    flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_INFREQUENT_RESUBMIT;

  if (normalized)
    flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_NORMALIZED;
  attribute->flags = flags;

  /* NB: get_gl_type_size must be called after setting the type
   * flags, above. */
  if (attribute->stride)
    attribute->span_bytes = buffer->n_vertices * attribute->stride;
  else
    attribute->span_bytes = buffer->n_vertices
			    * attribute->n_components
			    * get_gl_type_size (attribute->flags);

  if (!modifying_an_attrib)
    buffer->new_attributes =
      g_list_prepend (buffer->new_attributes, attribute);
}

void
cogl_vertex_buffer_delete (CoglHandle handle,
			   const char *attribute_name)
{
  CoglVertexBuffer *buffer;
  GQuark name = g_quark_from_string (attribute_name);
  GList *tmp;

  if (!cogl_is_vertex_buffer (handle))
    return;

  buffer = _cogl_vertex_buffer_pointer_from_handle (handle);

  /* The submit function works by diffing between submitted_attributes
   * and new_attributes to minimize the upload bandwidth + cost of
   * allocating new VBOs, so if there isn't already a list of new_attributes
   * we create one: */
  if (!buffer->new_attributes)
    buffer->new_attributes = copy_submitted_attributes_list (buffer);

  for (tmp = buffer->new_attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *submitted_attribute = tmp->data;
      if (submitted_attribute->name == name)
	{
	  buffer->new_attributes =
	    g_list_delete_link (buffer->new_attributes, tmp);
	  g_slice_free (CoglVertexBufferAttrib, submitted_attribute);
	  return;
	}
    }

  g_warning ("Failed to find an attribute named %s to delete\n",
	     attribute_name);
}

static void
set_attribute_enable (CoglHandle handle,
		      const char *attribute_name,
		      gboolean state)
{
  CoglVertexBuffer *buffer;
  GQuark name_quark = g_quark_from_string (attribute_name);
  GList *tmp;

  if (!cogl_is_vertex_buffer (handle))
    return;

  buffer = _cogl_vertex_buffer_pointer_from_handle (handle);

  /* NB: If a buffer is currently being edited, then there can be two seperate
   * lists of attributes; those that are currently submitted and a new list yet
   * to be submitted, we need to modify both. */

  for (tmp = buffer->new_attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;
      if (attribute->name == name_quark)
	{
	  if (state)
	    attribute->flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_ENABLED;
	  else
	    attribute->flags &= ~COGL_VERTEX_BUFFER_ATTRIB_FLAG_ENABLED;
	  break;
	}
    }

  for (tmp = buffer->submitted_vbos; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferVBO *cogl_vbo = tmp->data;
      GList *tmp2;

      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *attribute = tmp2->data;
	  if (attribute->name == name_quark)
	    {
	      if (state)
		attribute->flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_ENABLED;
	      else
		attribute->flags &= ~COGL_VERTEX_BUFFER_ATTRIB_FLAG_ENABLED;
	      return;
	    }
	}
    }

  g_warning ("Failed to find an attribute named %s to %s\n",
	     attribute_name,
	     state == TRUE ? "enable" : "disable");
}

void
cogl_vertex_buffer_enable (CoglHandle handle,
			       const char *attribute_name)
{
  set_attribute_enable (handle, attribute_name, TRUE);
}

void
cogl_vertex_buffer_disable (CoglHandle handle,
			    const char *attribute_name)
{
  set_attribute_enable (handle, attribute_name, FALSE);
}

static void
cogl_vertex_buffer_attribute_free (CoglVertexBufferAttrib *attribute)
{
  g_slice_free (CoglVertexBufferAttrib, attribute);
}

/* Given an attribute that we know has already been submitted before, this
 * function looks for the existing VBO that contains it.
 *
 * Note: It will free redundant attribute struct once the corresponding
 * VBO has been found.
 */
static void
filter_already_submitted_attribute (CoglVertexBufferAttrib *attribute,
				    GList **reuse_vbos,
				    GList **submitted_vbos)
{
  GList *tmp;

  /* First check the cogl_vbos we already know are being reused since we
   * are more likley to get a match here */
  for (tmp = *reuse_vbos; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferVBO *cogl_vbo = tmp->data;
      GList *tmp2;

      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *vbo_attribute = tmp2->data;

	  if (vbo_attribute->name == attribute->name)
	    {
	      vbo_attribute->flags &=
                ~COGL_VERTEX_BUFFER_ATTRIB_FLAG_UNUSED;
	      /* Note: we don't free the redundant attribute here, since it
	       * will be freed after all filtering in
               * cogl_vertex_buffer_submit */
	      return;
	    }
	}
    }

  for (tmp = *submitted_vbos; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferVBO *cogl_vbo = tmp->data;
      CoglVertexBufferAttrib *reuse_attribute = NULL;
      GList *tmp2;

      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *vbo_attribute = tmp2->data;
	  if (vbo_attribute->name == attribute->name)
	    {
	      reuse_attribute = vbo_attribute;
	      /* Note: we don't free the redundant attribute here, since it
	       * will be freed after all filtering in
               * cogl_vertex_buffer_submit */

	      *submitted_vbos = g_list_remove_link (*submitted_vbos, tmp);
	      tmp->next = *reuse_vbos;
	      *reuse_vbos = tmp;
	      break;
	    }
	}

      if (!reuse_attribute)
	continue;

      /* Mark all but the matched attribute as UNUSED, so that when we
       * finish filtering all our attributes any attrributes still
       * marked as UNUSED can be removed from the their cogl_vbo */
      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *vbo_attribute = tmp2->data;
	  if (vbo_attribute != reuse_attribute)
	    vbo_attribute->flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_UNUSED;
	}

      return;
    }

  g_critical ("Failed to find the cogl vbo that corresponds to an\n"
	      "attribute that had apparently already been submitted!");
}

/* When we first mark a CoglVertexBufferVBO to be reused, we mark the
 * attributes as unsed, so that when filtering of attributes into VBOs is done
 * we can then prune the now unsed attributes. */
static void
remove_unused_attributes (CoglVertexBufferVBO *cogl_vbo)
{
  GList *tmp;
  GList *next;

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;
      next = tmp->next;

      if (attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_UNUSED)
	{
	  cogl_vbo->attributes =
	    g_list_delete_link (cogl_vbo->attributes, tmp);
	  g_slice_free (CoglVertexBufferAttrib, attribute);
	}
    }
}

/* Give a newly added, strided, attribute, this function looks for a
 * CoglVertexBufferVBO that the attribute is interleved with. If it can't
 * find one then a new CoglVertexBufferVBO is allocated and added to the
 * list of new_strided_vbos.
 */
static void
filter_strided_attribute (CoglVertexBufferAttrib *attribute,
			  GList **new_vbos)
{
  GList *tmp;
  CoglVertexBufferVBO *new_cogl_vbo;

  for (tmp = *new_vbos; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferVBO *cogl_vbo = tmp->data;
      GList *tmp2;

      if (!(cogl_vbo->flags & COGL_VERTEX_BUFFER_VBO_FLAG_STRIDED))
	continue;

      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *vbo_attribute = tmp2->data;
	  const char *attribute_start = attribute->u.pointer;
	  const char *vbo_attribute_start = vbo_attribute->u.pointer;

	  /* NB: All attributes have buffer->n_vertices values which
	   * simplifies determining which attributes are interleved
	   * since we assume they will start no farther than +- a
	   * stride away from each other:
	   */
	  if (attribute_start <= (vbo_attribute_start - vbo_attribute->stride)
	      || attribute_start
		 >= (vbo_attribute_start + vbo_attribute->stride))
	    continue; /* Not interleved */

	  cogl_vbo->attributes =
	    g_list_prepend (cogl_vbo->attributes, attribute);

	  if (attribute->flags &
              COGL_VERTEX_BUFFER_ATTRIB_FLAG_FREQUENT_RESUBMIT)
	    {
	      cogl_vbo->flags &=
                ~COGL_VERTEX_BUFFER_VBO_FLAG_INFREQUENT_RESUBMIT;
	      cogl_vbo->flags |=
                COGL_VERTEX_BUFFER_VBO_FLAG_FREQUENT_RESUBMIT;
	    }
	      return;
	}
    }
  new_cogl_vbo = g_slice_alloc (sizeof (CoglVertexBufferVBO));
  new_cogl_vbo->vbo_name = NULL;
  new_cogl_vbo->attributes = NULL;
  new_cogl_vbo->attributes =
    g_list_prepend (new_cogl_vbo->attributes, attribute);
  /* Any one of the interleved attributes will have the same span_bytes */
  new_cogl_vbo->vbo_bytes = attribute->span_bytes;
  new_cogl_vbo->flags = COGL_VERTEX_BUFFER_VBO_FLAG_STRIDED;

  if (attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_INFREQUENT_RESUBMIT)
    new_cogl_vbo->flags |= COGL_VERTEX_BUFFER_VBO_FLAG_INFREQUENT_RESUBMIT;
  else
    new_cogl_vbo->flags |= COGL_VERTEX_BUFFER_VBO_FLAG_FREQUENT_RESUBMIT;

  *new_vbos = g_list_prepend (*new_vbos, new_cogl_vbo);
  return;
}

/* This iterates through the list of submitted VBOs looking for one that
 * contains attribute. If found the list *link* is removed and returned */
static GList *
unlink_submitted_vbo_containing_attribute (GList **submitted_vbos,
					   CoglVertexBufferAttrib *attribute)
{
  GList *tmp;
  GList *next = NULL;

  for (tmp = *submitted_vbos; tmp != NULL; tmp = next)
    {
      CoglVertexBufferVBO *submitted_vbo = tmp->data;
      GList *tmp2;

      next = tmp->next;

      for (tmp2 = submitted_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *submitted_attribute = tmp2->data;

	  if (submitted_attribute->name == attribute->name)
	    {
	      *submitted_vbos = g_list_remove_link (*submitted_vbos, tmp);
	      return tmp;
	    }
	}
    }

  return NULL;
}

/* Unlinks all the submitted VBOs that conflict with the new cogl_vbo and
 * returns them as a list. */
static GList *
get_submitted_vbo_conflicts (GList **submitted_vbos,
                             CoglVertexBufferVBO *cogl_vbo)
{
  GList *tmp;
  GList *conflicts = NULL;

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = tmp->next)
    {
      GList *link =
	unlink_submitted_vbo_containing_attribute (submitted_vbos,
						   tmp->data);
      if (link)
	{
	  /* prepend the link to the list of conflicts: */
	  link->next = conflicts;
	  conflicts = link;
	}
    }
  return conflicts;
}

/* Any attributes in cogl_vbo gets removed from conflict_vbo */
static void
disassociate_conflicting_attributes (CoglVertexBufferVBO *conflict_vbo,
				     CoglVertexBufferVBO *cogl_vbo)
{
  GList *tmp;

  /* NB: The attributes list in conflict_vbo will be shrinking so
   * we iterate those in the inner loop. */

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;
      GList *tmp2;
      for (tmp2 = conflict_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *conflict_attribute = tmp2->data;

	  if (conflict_attribute->name == attribute->name)
	    {
	      cogl_vertex_buffer_attribute_free (conflict_attribute);
	      conflict_vbo->attributes =
		g_list_delete_link (conflict_vbo->attributes, tmp2);
	      break;
	    }
	}
    }
}

static void
cogl_vertex_buffer_vbo_free (CoglVertexBufferVBO *cogl_vbo,
                             gboolean delete_gl_vbo)
{
  GList *tmp;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = tmp->next)
    {
      cogl_vertex_buffer_attribute_free (tmp->data);
    }
  g_list_free (cogl_vbo->attributes);

  if (delete_gl_vbo && cogl_vbo->flags &
      COGL_VERTEX_BUFFER_VBO_FLAG_SUBMITTED)
    {
      if (cogl_get_features () & COGL_FEATURE_VBOS)
	GE (glDeleteBuffers (1, (GLuint *)&cogl_vbo->vbo_name));
      else
	g_free (cogl_vbo->vbo_name);
    }

  g_slice_free (CoglVertexBufferVBO, cogl_vbo);
}

/* This figures out the lowest attribute client pointer. (This pointer is used
 * to upload all the interleved attributes).
 *
 * In the process it also replaces the client pointer with the attributes
 * offset, and marks the attribute as submitted.
 */
static const void *
prep_strided_vbo_for_upload (CoglVertexBufferVBO *cogl_vbo)
{
  GList *tmp;
  const char *lowest_pointer = NULL;

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;
      const char *client_pointer = attribute->u.pointer;

      if (!lowest_pointer || client_pointer < lowest_pointer)
	lowest_pointer = client_pointer;
    }

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;
      const char *client_pointer = attribute->u.pointer;
      attribute->u.vbo_offset = client_pointer - lowest_pointer;
      attribute->flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_SUBMITTED;
    }

  return lowest_pointer;
}

static gboolean
upload_multipack_vbo_via_map_buffer (CoglVertexBufferVBO *cogl_vbo)
{
#if HAVE_COGL_GL
  GList *tmp;
  unsigned int offset = 0;
  char *buf;
  gboolean fallback =
    (cogl_get_features () & COGL_FEATURE_VBOS) ? FALSE : TRUE;

  _COGL_GET_CONTEXT (ctx, FALSE);

  if (!fallback)
    {
      buf = glMapBuffer (GL_ARRAY_BUFFER, GL_WRITE_ONLY);
      glGetError();
    }
  else
    buf = cogl_vbo->vbo_name;

  if (!buf)
    return FALSE;

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;
      gsize attribute_size = attribute->span_bytes;
      gsize gl_type_size = get_gl_type_size (attribute->flags);

      PAD_FOR_ALIGNMENT (offset, gl_type_size);

      memcpy (buf + offset, attribute->u.pointer, attribute_size);

      attribute->u.vbo_offset = offset;
      attribute->flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_SUBMITTED;
      offset += attribute_size;
    }

  if (!fallback)
    glUnmapBuffer (GL_ARRAY_BUFFER);

  return TRUE;
#else
  return FALSE;
#endif
}

static void
upload_multipack_vbo_via_buffer_sub_data (CoglVertexBufferVBO *cogl_vbo)
{
  GList *tmp;
  unsigned int offset = 0;
  gboolean fallback =
    (cogl_get_features () & COGL_FEATURE_VBOS) ? FALSE : TRUE;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  for (tmp = cogl_vbo->attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;
      gsize attribute_size = attribute->span_bytes;
      gsize gl_type_size = get_gl_type_size (attribute->flags);

      PAD_FOR_ALIGNMENT (offset, gl_type_size);

      if (!fallback)
	{
	  GE (glBufferSubData (GL_ARRAY_BUFFER,
			       offset,
			       attribute_size,
			       attribute->u.pointer));
	}
      else
	{
	  char *dest = (char *)cogl_vbo->vbo_name + offset;
	  memcpy (dest, attribute->u.pointer, attribute_size);
	}

      attribute->u.vbo_offset = offset;
      attribute->flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_SUBMITTED;
      offset += attribute_size;
    }
}

static void
upload_gl_vbo (CoglVertexBufferVBO *cogl_vbo)
{
  GLenum usage;
  gboolean fallback =
    (cogl_get_features () & COGL_FEATURE_VBOS) ? FALSE : TRUE;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (cogl_vbo->flags & COGL_VERTEX_BUFFER_VBO_FLAG_FREQUENT_RESUBMIT)
    usage = GL_DYNAMIC_DRAW;
  else
    usage = GL_STATIC_DRAW;

  if (!fallback)
    {
      g_return_if_fail (cogl_vbo->vbo_name != NULL);

      GE (glBindBuffer (GL_ARRAY_BUFFER,
			GPOINTER_TO_UINT (cogl_vbo->vbo_name)));
    }
  else if (cogl_vbo->vbo_name == NULL)
    {
      /* If the driver doesn't support VBOs then we simply allocate
       * a client side fake vbo buffer. Unlike VBOs we can't allocate
       * without specifying a size which is why we defer allocation
       * until here. */
      cogl_vbo->vbo_name = g_malloc (cogl_vbo->vbo_bytes);
    }

  if (cogl_vbo->flags & COGL_VERTEX_BUFFER_VBO_FLAG_STRIDED)
    {
      const void *pointer =
	prep_strided_vbo_for_upload (cogl_vbo);
      if (!fallback)
	{
	  GE (glBufferData (GL_ARRAY_BUFFER,
			    cogl_vbo->vbo_bytes,
			    pointer,
			    usage));
	}
      else
	memcpy (cogl_vbo->vbo_name, pointer, cogl_vbo->vbo_bytes);
    }
  else if (cogl_vbo->flags & COGL_VERTEX_BUFFER_VBO_FLAG_MULTIPACK)
    {
      /* First we make it obvious to the driver that we want to update the
       * whole buffer (without this, the driver is more likley to block
       * if the GPU is busy using the buffer) */
      if (!fallback)
	{
	  GE (glBufferData (GL_ARRAY_BUFFER,
			    cogl_vbo->vbo_bytes,
			    NULL,
			    usage));
	}

      /* I think it might depend on the specific driver/HW whether its better
       * to use glMapBuffer here or glBufferSubData here. There is even a good
       * thread about this topic here:
       * http://www.mail-archive.com/dri-devel@lists.sourceforge.net/msg35004.html
       * For now I have gone with glMapBuffer, but the jury is still out.
       */

      if (!upload_multipack_vbo_via_map_buffer (cogl_vbo))
	upload_multipack_vbo_via_buffer_sub_data  (cogl_vbo);
    }
  else
    {
      CoglVertexBufferAttrib *attribute = cogl_vbo->attributes->data;
      if (!fallback)
	{
	  GE (glBufferData (GL_ARRAY_BUFFER,
			    cogl_vbo->vbo_bytes,
			    attribute->u.pointer,
			    usage));
	}
      else
	memcpy (cogl_vbo->vbo_name, attribute->u.pointer, cogl_vbo->vbo_bytes);

      /* We forget this pointer now since the client will be free
       * to re-use this memory */
      attribute->u.pointer = NULL;
      attribute->flags |= COGL_VERTEX_BUFFER_ATTRIB_FLAG_SUBMITTED;
    }

  cogl_vbo->flags |= COGL_VERTEX_BUFFER_VBO_FLAG_SUBMITTED;

  if (!fallback)
    GE (glBindBuffer (GL_ARRAY_BUFFER, 0));
}

/* Note: although there ends up being quite a few inner loops involved with
 * resolving buffers, the number of attributes will be low so I don't expect
 * them to cause a problem. */
static void
cogl_vertex_buffer_vbo_resolve (CoglVertexBuffer *buffer,
			        CoglVertexBufferVBO *new_cogl_vbo,
			        GList **final_vbos)
{
  GList *conflicts;
  GList *tmp;
  GList *next;
  gboolean found_target_vbo = FALSE;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  conflicts =
    get_submitted_vbo_conflicts (&buffer->submitted_vbos, new_cogl_vbo);

  for (tmp = conflicts; tmp != NULL; tmp = next)
    {
      CoglVertexBufferVBO *conflict_vbo = tmp->data;

      next = tmp->next;

      disassociate_conflicting_attributes (conflict_vbo, new_cogl_vbo);

      if (!conflict_vbo->attributes)
	{
	  /* See if we can re-use this now empty VBO: */

	  if (!found_target_vbo
	      && conflict_vbo->vbo_bytes == new_cogl_vbo->vbo_bytes)
	    {
	      found_target_vbo = TRUE;
	      new_cogl_vbo->vbo_name = conflict_vbo->vbo_name;
	      cogl_vertex_buffer_vbo_free (conflict_vbo, FALSE);

	      upload_gl_vbo (new_cogl_vbo);

	      *final_vbos = g_list_prepend (*final_vbos, new_cogl_vbo);
	    }
	  else
	    cogl_vertex_buffer_vbo_free (conflict_vbo, TRUE);
	}
      else
	{
	  /* Relink the VBO back into buffer->submitted_vbos since it may
	   * be involved in other conflicts later */
	  tmp->next = buffer->submitted_vbos;
	  tmp->prev = NULL;
	  buffer->submitted_vbos = tmp;
	}
    }

  if (!found_target_vbo)
    {
      if (cogl_get_features () & COGL_FEATURE_VBOS)
	GE (glGenBuffers (1, (GLuint *)&new_cogl_vbo->vbo_name));
      else
	new_cogl_vbo->vbo_name = NULL;
        /* this will be allocated at upload time */

      upload_gl_vbo (new_cogl_vbo);
      *final_vbos = g_list_prepend (*final_vbos, new_cogl_vbo);
    }
}

static void
cogl_vertex_buffer_submit_real (CoglVertexBuffer *buffer)
{
  GList *tmp;
  CoglVertexBufferVBO *new_multipack_vbo;
  GList *new_multipack_vbo_link;
  GList *new_vbos = NULL;
  GList *reuse_vbos = NULL;
  GList *final_vbos = NULL;

  if (!buffer->new_attributes)
    return;

  /* The objective now is to copy the attribute data supplied by the client
   * into buffer objects, but it's important to minimize the number of
   * redundant data uploads.
   *
   * We obviously aim to group together the attributes that are interleved so
   * that they can be delivered in one go to the driver.
   * All BOs for interleved data are created as STATIC_DRAW_ARB.
   *
   * Non interleved attributes tagged as INFREQUENT_RESUBMIT will be grouped
   * together back to back in a single BO created as STATIC_DRAW_ARB
   *
   * Non interleved attributes tagged as FREQUENT_RESUBMIT will be copied into
   * individual buffer objects, and the BO itself created DYNAMIC_DRAW_ARB
   *
   * If we are modifying a previously submitted CoglVertexBuffer then we are
   * carefull not to needlesly delete OpenGL buffer objects and replace with
   * new ones, instead we upload new data to the existing buffers.
   */

  /* NB: We must forget attribute->pointer after submitting since the user
   * is free to re-use that memory for other purposes now. */

  /* Pseudo code:
   *
   * Broadly speaking we start with a list of unsorted attributes, and filter
   * those into 'new' and 're-use' CoglVertexBufferVBO (CBO) lists. We then
   * take the list of new CBO structs and compare with the CBOs that have
   * already been submitted to the GPU (but ignoring those we already know will
   * be re-used) to determine what other CBOs can be re-used, due to being
   * superseded, and what new GL VBOs need to be created.
   *
   * We have three kinds of CBOs:
   * - Unstrided CBOs
   *    These contain a single tightly packed attribute
   *    These are currently the only ones ever marked as FREQUENT_SUBMIT
   * - Strided CBOs
   *	 These typically contain multiple interleved sets of attributes,
   *	 though they can contain just one attribute with a stride
   * - Multi Pack CBOs
   *     These contain multiple attributes tightly packed back to back)
   *
   * First create a new-CBOs entry "new-multipack-CBO"
   * Tag "new-multipack-CBO" as MULTIPACK + INFREQUENT_RESUBMIT
   * For each unsorted attrib:
   *   if already marked as submitted:
   *	 iterate reuse-CBOs:
   *	   if we find one that contains this attribute:
   *	     free redundant unsorted attrib struct
   *	     remove the UNUSED flag from the attrib found in the reuse-CBO
   *	     continue to next unsorted attrib
   *	 iterate submitted VBOs:
   *	   if we find one that contains this attribute:
   *	     free redundant unsorted attrib struct
   *	     unlink the vbo and move it to the list of reuse-CBOs
   *	     mark all attributes except the one just matched as UNUSED
   *	 assert (found)
   *	 continue to next unsorted attrib
   *   if strided:
   * 	 iterate the new, strided, CBOs, to see if the attribute is
   *	 interleved with one of them, if found:
   *	   add to the matched CBO
   *	 else if not found:
   *	   create a new-CBOs entry tagged STRIDED + INFREQUENT_RESUBMIT
   *   else if unstrided && tagged with FREQUENT_RESUBMIT:
   *     create a new-CBOs entry tagged UNSTRIDED + FREQUENT_RESUBMIT
   *   else
   *     add to the new-multipack-CBO
   * free list of unsorted-attribs
   *
   * Next compare the new list of CBOs with the submitted set and try to
   * minimize the memory bandwidth required to upload the attributes and the
   * overhead of creating new GL-BOs.
   *
   * We deal with four sets of CBOs:
   * - The "new" CBOs
   *    (as determined above during filtering)
   * - The "re-use" CBOs
   *    (as determined above during filtering)
   * - The "submitted" CBOs
   *    (I.e. ones currently submitted to the GPU)
   * - The "final" CBOs
   *	(The result of resolving the differences between the above sets)
   *
   * The re-use CBOs are dealt with first, and we simply delete any remaining
   * attributes in these that are still marked as UNUSED, and move them
   * to the list of final CBOs.
   *
   * Next we iterate through the "new" CBOs, searching for conflicts
   * with the "submitted" CBOs and commit our decision to the "final" CBOs
   *
   * When searching for submitted entries we always unlink items from the
   * submitted list once we make matches (before we make descisions
   * based on the matches). If the CBO node is superseded it is freed,
   * if it is modified but may be needed for more descisions later it is
   * relinked back into the submitted list and if it's identical to a new
   * CBO it will be linked into the final list.
   *
   * At the end the list of submitted CBOs represents the attributes that were
   * deleted from the buffer.
   *
   * Iterate re-use-CBOs:
   *   Iterate attribs for each:
   *	 if attrib UNUSED:
   *	   remove the attrib from the CBO + free
   *	   |Note: we could potentially mark this as a re-useable gap
   *	   |if needs be later.
   *   add re-use CBO to the final-CBOs
   * Iterate new-CBOs:
   *   List submitted CBOs conflicting with the this CBO (Unlinked items)
   *   found-target-BO=FALSE
   *   Iterate conflicting CBOs:
   *	 Disassociate conflicting attribs from conflicting CBO struct
   *	 If no attribs remain:
   *	   If found-target-BO!=TRUE
   *	   _AND_ If the total size of the conflicting CBO is compatible:
   *	   |Note: We don't currently consider re-using oversized buffers
   *	     found-target-BO=TRUE
   *	     upload replacement data
   *	     free submitted CBO struct
   *	     add new CBO struct to final-CBOs
   *	   else:
   *	     delete conflict GL-BO
   *	     delete conflict CBO struct
   *	 else:
   *	   relink CBO back into submitted-CBOs
   *
   *   if found-target-BO == FALSE:
   *	 create a new GL-BO
   *	 upload data
   *	 add new CBO struct to final-BOs
   *
   * Iterate through the remaining "submitted" CBOs:
   *   delete the submitted GL-BO
   *   free the submitted CBO struct
   */

  new_multipack_vbo = g_slice_alloc (sizeof (CoglVertexBufferVBO));
  new_multipack_vbo->vbo_name = NULL;
  new_multipack_vbo->flags =
    COGL_VERTEX_BUFFER_VBO_FLAG_MULTIPACK
    | COGL_VERTEX_BUFFER_VBO_FLAG_INFREQUENT_RESUBMIT;
  new_multipack_vbo->vbo_bytes = 0;
  new_multipack_vbo->attributes = NULL;
  new_vbos = g_list_prepend (new_vbos, new_multipack_vbo);
  /* We save the link pointer here, just so we can do a fast removal later if
   * no attributes get added to this vbo. */
  new_multipack_vbo_link = new_vbos;

  /* Start with a list of unsorted attributes, and filter those into
   * potential new Cogl BO structs
   */
  for (tmp = buffer->new_attributes; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferAttrib *attribute = tmp->data;

      if (attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_SUBMITTED)
	{
	  /* If the attribute is already marked as submitted, then we need
	   * to find the existing VBO that contains it so we dont delete it.
	   *
	   * NB: this also frees the attribute struct since it's implicitly
	   * redundant in this case.
	   */
	  filter_already_submitted_attribute (attribute,
					      &reuse_vbos,
					      &buffer->submitted_vbos);
	}
      else if (attribute->stride)
	{
	  /* look for a CoglVertexBufferVBO that the attribute is
           * interleved with. If one can't be found then a new
           * CoglVertexBufferVBO is allocated and added to the list of
           * new_vbos: */
	  filter_strided_attribute (attribute, &new_vbos);
	}
      else if (attribute->flags &
               COGL_VERTEX_BUFFER_ATTRIB_FLAG_FREQUENT_RESUBMIT)
	{
	  CoglVertexBufferVBO *cogl_vbo =
            g_slice_alloc (sizeof (CoglVertexBufferVBO));

	  /* attributes we expect will be frequently resubmitted are placed
	   * in their own VBO so that updates don't impact other attributes
	   */

	  cogl_vbo->vbo_name = NULL;
	  cogl_vbo->flags =
            COGL_VERTEX_BUFFER_VBO_FLAG_UNSTRIDED
	    | COGL_VERTEX_BUFFER_VBO_FLAG_FREQUENT_RESUBMIT;
	  cogl_vbo->attributes = NULL;
	  cogl_vbo->attributes = g_list_prepend (cogl_vbo->attributes,
						 attribute);
	  cogl_vbo->vbo_bytes = attribute->span_bytes;
	  new_vbos = g_list_prepend (new_vbos, cogl_vbo);
	}
      else
	{
	  gsize gl_type_size = get_gl_type_size (attribute->flags);

	  /* Infrequently updated attributes just get packed back to back
	   * in a single VBO: */
	  new_multipack_vbo->attributes =
	    g_list_prepend (new_multipack_vbo->attributes,
			    attribute);

	  /* Note: we have to ensure that each run of attributes is
	   * naturally aligned according to its data type, which may
	   * require some padding bytes: */

	  /* XXX: We also have to be sure that the attributes aren't
	   * reorderd before being uploaded because the alignment padding
	   * is based on the adjacent attribute.
	   */

	  PAD_FOR_ALIGNMENT (new_multipack_vbo->vbo_bytes, gl_type_size);

	  new_multipack_vbo->vbo_bytes += attribute->span_bytes;
	}
    }

  /* At this point all buffer->new_attributes have been filtered into
   * CoglVertexBufferVBOs... */
  g_list_free (buffer->new_attributes);
  buffer->new_attributes = NULL;

  /* If the multipack vbo wasn't needed: */
  if (new_multipack_vbo->attributes == NULL)
    {
      new_vbos = g_list_delete_link (new_vbos, new_multipack_vbo_link);
      g_slice_free (CoglVertexBufferVBO, new_multipack_vbo);
    }

  for (tmp = reuse_vbos; tmp != NULL; tmp = tmp->next)
    remove_unused_attributes (tmp->data);
  final_vbos = g_list_concat (final_vbos, reuse_vbos);

  for (tmp = new_vbos; tmp != NULL; tmp = tmp->next)
    cogl_vertex_buffer_vbo_resolve (buffer, tmp->data, &final_vbos);

  /* Anything left corresponds to deleted attributes: */
  for (tmp = buffer->submitted_vbos; tmp != NULL; tmp = tmp->next)
    cogl_vertex_buffer_vbo_free (tmp->data, TRUE);
  g_list_free (buffer->submitted_vbos);
  g_list_free (new_vbos);

  buffer->submitted_vbos = final_vbos;
}

void
cogl_vertex_buffer_submit (CoglHandle handle)
{
  CoglVertexBuffer *buffer;

  if (!cogl_is_vertex_buffer (handle))
    return;

  buffer = _cogl_vertex_buffer_pointer_from_handle (handle);

  cogl_vertex_buffer_submit_real (buffer);
}

static GLenum
get_gl_type_from_attribute_flags (CoglVertexBufferAttribFlags flags)
{
  CoglVertexBufferAttribFlags gl_type =
    flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_MASK;

  switch (gl_type)
    {
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_BYTE:
      return GL_BYTE;
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_BYTE:
      return GL_UNSIGNED_BYTE;
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_SHORT:
      return GL_SHORT;
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_SHORT:
      return GL_UNSIGNED_SHORT;
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_FLOAT:
      return GL_FLOAT;
#if HAVE_COGL_GL
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_INT:
      return GL_INT;
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_UNSIGNED_INT:
      return GL_UNSIGNED_INT;
    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_GL_TYPE_DOUBLE:
      return GL_DOUBLE;
#endif
    default:
      g_warning ("Couldn't convert from attribute flags (0x%08x) "
		 "to gl type enum\n", flags);
      return 0;
    }
}

static CoglHandle
enable_state_for_drawing_buffer (CoglVertexBuffer *buffer)
{
  GList       *tmp;
  GLenum       gl_type;
#ifdef MAY_HAVE_PROGRAMABLE_GL
  GLuint       generic_index = 0;
#endif
  unsigned long enable_flags = 0;
  const GList *layers;
  guint32      fallback_layers = 0;
  int          i;
  gboolean     skip_gl_color = FALSE;
  CoglMaterialFlushOptions options;
  CoglHandle   source;

  _COGL_GET_CONTEXT (ctx, COGL_INVALID_HANDLE);

  source = ctx->source_material;

  if (buffer->new_attributes)
    cogl_vertex_buffer_submit_real (buffer);

  options.flags =
    COGL_MATERIAL_FLUSH_FALLBACK_MASK;
  memset (&options.wrap_mode_overrides, 0,
          sizeof (options.wrap_mode_overrides));

  _cogl_bitmask_clear_all (&ctx->temp_bitmask);

  for (tmp = buffer->submitted_vbos; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferVBO *cogl_vbo = tmp->data;
      GList *tmp2;
      char *base;
      const GLvoid *pointer;

      if (cogl_get_features () & COGL_FEATURE_VBOS)
	{
	  GE (glBindBuffer (GL_ARRAY_BUFFER,
			    GPOINTER_TO_UINT (cogl_vbo->vbo_name)));
	  base = NULL;
	}
      else
	base = cogl_vbo->vbo_name;

      /* When GL VBOs are bing used then the "pointer" we pass to
       * glColorPointer glVertexAttribPointer etc is actually an offset into
       * the currently bound VBO.
       *
       * If we don't have VBO support though, then we must point into
       * our fake client side VBO.
       */

      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *attribute = tmp2->data;
	  CoglVertexBufferAttribFlags type =
	    attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_TYPE_MASK;

	  if (!(attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_ENABLED))
	    continue;

	  gl_type = get_gl_type_from_attribute_flags (attribute->flags);
	  switch (type)
	    {
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_COLOR_ARRAY:
	      enable_flags |= COGL_ENABLE_COLOR_ARRAY;
	      /* GE (glEnableClientState (GL_COLOR_ARRAY)); */
	      pointer = (const GLvoid *)(base + attribute->u.vbo_offset);
	      GE (glColorPointer (attribute->n_components,
				  gl_type,
				  attribute->stride,
				  pointer));

              if (!_cogl_material_get_real_blend_enabled (ctx->source_material))
                {
                  CoglMaterialBlendEnable blend_enable =
                    COGL_MATERIAL_BLEND_ENABLE_ENABLED;
                  source = cogl_material_copy (ctx->source_material);
                  _cogl_material_set_blend_enabled (source, blend_enable);
                  skip_gl_color = TRUE;
                }
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_NORMAL_ARRAY:
	      /* FIXME: go through cogl cache to enable normal array */
	      GE (glEnableClientState (GL_NORMAL_ARRAY));
	      pointer = (const GLvoid *)(base + attribute->u.vbo_offset);
	      GE (glNormalPointer (gl_type,
				   attribute->stride,
				   pointer));
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_TEXTURE_COORD_ARRAY:
              GE (glClientActiveTexture (GL_TEXTURE0 +
                                         attribute->texture_unit));
              GE (glEnableClientState (GL_TEXTURE_COORD_ARRAY));
	      pointer = (const GLvoid *)(base + attribute->u.vbo_offset);
	      GE (glTexCoordPointer (attribute->n_components,
				     gl_type,
				     attribute->stride,
				     pointer));
              _cogl_bitmask_set (&ctx->temp_bitmask,
                                 attribute->texture_unit, TRUE);
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_VERTEX_ARRAY:
	      enable_flags |= COGL_ENABLE_VERTEX_ARRAY;
	      /* GE (glEnableClientState (GL_VERTEX_ARRAY)); */
	      pointer = (const GLvoid *)(base + attribute->u.vbo_offset);
	      GE (glVertexPointer (attribute->n_components,
				   gl_type,
				   attribute->stride,
				   pointer));
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_CUSTOM_ARRAY:
	      {
#ifdef MAY_HAVE_PROGRAMABLE_GL
		GLboolean normalized = GL_FALSE;
		if (attribute->flags &
                    COGL_VERTEX_BUFFER_ATTRIB_FLAG_NORMALIZED)
		  normalized = GL_TRUE;
		/* FIXME: go through cogl cache to enable generic array */
		GE (glEnableVertexAttribArray (generic_index++));
		pointer = (const GLvoid *)(base + attribute->u.vbo_offset);
		GE (glVertexAttribPointer (generic_index,
					   attribute->n_components,
					   gl_type,
					   normalized,
					   attribute->stride,
					   pointer));
#endif
	      }
	      break;
	    default:
	      g_warning ("Unrecognised attribute type 0x%08x", type);
	    }
	}
    }

  layers = cogl_material_get_layers (source);
  for (tmp = (GList *)layers, i = 0;
       tmp != NULL;
       tmp = tmp->next, i++)
    {
      CoglHandle layer = (CoglHandle)tmp->data;
      CoglHandle tex_handle = cogl_material_layer_get_texture (layer);

      /* invalid textures will be handled correctly in
       * _cogl_material_flush_layers_gl_state */
      if (tex_handle == COGL_INVALID_HANDLE)
        continue;

      if (!cogl_material_get_layer_point_sprite_coords_enabled (source, i))
        {
          /* By default COGL_MATERIAL_WRAP_MODE_AUTOMATIC becomes
             GL_CLAMP_TO_EDGE but we want GL_REPEAT to maintain
             compatibility with older versions of Cogl so we'll
             override it. We don't want to do this for point sprites
             because in that case the whole texture is drawn so you
             would usually want clamp-to-edge. */
          if (cogl_material_layer_get_wrap_mode_s (layer) ==
              COGL_MATERIAL_WRAP_MODE_AUTOMATIC)
            {
              options.wrap_mode_overrides.values[i].s =
                COGL_MATERIAL_WRAP_MODE_OVERRIDE_REPEAT;
              options.flags |= COGL_MATERIAL_FLUSH_WRAP_MODE_OVERRIDES;
            }
          if (cogl_material_layer_get_wrap_mode_t (layer) ==
              COGL_MATERIAL_WRAP_MODE_AUTOMATIC)
            {
              options.wrap_mode_overrides.values[i].t =
                COGL_MATERIAL_WRAP_MODE_OVERRIDE_REPEAT;
              options.flags |= COGL_MATERIAL_FLUSH_WRAP_MODE_OVERRIDES;
            }
          if (cogl_material_layer_get_wrap_mode_p (layer) ==
              COGL_MATERIAL_WRAP_MODE_AUTOMATIC)
            {
              options.wrap_mode_overrides.values[i].p =
                COGL_MATERIAL_WRAP_MODE_OVERRIDE_REPEAT;
              options.flags |= COGL_MATERIAL_FLUSH_WRAP_MODE_OVERRIDES;
            }
        }

      /* Give the texture a chance to know that we're rendering
         non-quad shaped primitives. If the texture is in an atlas it
         will be migrated */
      _cogl_texture_ensure_non_quad_rendering (tex_handle);

      /* We need to ensure the mipmaps are ready before deciding
       * anything else about the texture because the texture storate
       * could completely change if it needs to be migrated out of the
       * atlas and will affect how we validate the layer.
       */
      _cogl_material_layer_pre_paint (layer);

      if (!_cogl_texture_can_hardware_repeat (tex_handle))
        {
          g_warning ("Disabling layer %d of the current source material, "
                     "because texturing with the vertex buffer API is not "
                     "currently supported using sliced textures, or textures "
                     "with waste\n", i);

          /* XXX: maybe we can add a mechanism for users to forcibly use
           * textures with waste where it would be their responsability to use
           * texture coords in the range [0,1] such that sampling outside isn't
           * required. We can then use a texture matrix (or a modification of
           * the users own matrix) to map 1 to the edge of the texture data.
           *
           * Potentially, given the same guarantee as above we could also
           * support a single sliced layer too. We would have to redraw the
           * vertices once for each layer, each time with a fiddled texture
           * matrix.
           */
          fallback_layers |= (1 << i);
        }
    }

  /* Disable any tex coord arrays that we didn't use */
  _cogl_disable_other_texcoord_arrays (&ctx->temp_bitmask);

  /* NB: _cogl_framebuffer_flush_state may disrupt various state (such
   * as the material state) when flushing the clip stack, so should
   * always be done first when preparing to draw. */
  _cogl_framebuffer_flush_state (_cogl_get_framebuffer (), 0);

  options.fallback_layers = fallback_layers;

  if (G_UNLIKELY (options.flags))
    {
      /* If we haven't already created a derived material... */
      if (source == ctx->source_material)
        source = cogl_material_copy (ctx->source_material);
      _cogl_material_apply_overrides (source, &options);

      /* TODO:
       * overrides = cogl_material_get_data (material,
       *                                     last_overrides_key);
       * if (overrides)
       *   {
       *     age = cogl_material_get_age (material);
       *     XXX: actually we also need to check for legacy_state
       *     and blending overrides for use of glColorPointer...
       *     if (overrides->ags != age ||
       *         memcmp (&overrides->options, &options,
       *                 sizeof (options) != 0)
       *       {
       *         cogl_handle_unref (overrides->weak_material);
       *         g_slice_free (Overrides, overrides);
       *         overrides = NULL;
       *       }
       *   }
       * if (!overrides)
       *   {
       *     overrides = g_slice_new (Overrides);
       *     overrides->weak_material =
       *       cogl_material_weak_copy (ctx->source_material);
       *     _cogl_material_apply_overrides (overrides->weak_material,
       *                                     &options);
       *
       *     cogl_material_set_data (material, last_overrides_key,
       *                             weak_overrides,
       *                             free_overrides_cb,
       *                             NULL);
       *   }
       * source = overrides->weak_material;
       */
    }

  if (G_UNLIKELY (ctx->legacy_state_set))
    {
      /* If we haven't already created a derived material... */
      if (source == ctx->source_material)
        source = cogl_material_copy (ctx->source_material);
      _cogl_material_apply_legacy_state (source);
    }

  _cogl_material_flush_gl_state (source, skip_gl_color);

  if (ctx->enable_backface_culling)
    enable_flags |= COGL_ENABLE_BACKFACE_CULLING;

  _cogl_enable (enable_flags);
  _cogl_flush_face_winding ();

  return source;
}

static void
disable_state_for_drawing_buffer (CoglVertexBuffer *buffer,
                                  CoglHandle source)
{
  GList *tmp;
  GLenum gl_type;
#ifdef MAY_HAVE_PROGRAMABLE_GL
  GLuint generic_index = 0;
#endif

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (G_UNLIKELY (source != ctx->source_material))
    cogl_handle_unref (source);

  /* Disable all the client state that cogl doesn't currently know
   * about:
   */
  if (cogl_get_features () & COGL_FEATURE_VBOS)
    GE (glBindBuffer (GL_ARRAY_BUFFER, 0));

  for (tmp = buffer->submitted_vbos; tmp != NULL; tmp = tmp->next)
    {
      CoglVertexBufferVBO *cogl_vbo = tmp->data;
      GList *tmp2;

      for (tmp2 = cogl_vbo->attributes; tmp2 != NULL; tmp2 = tmp2->next)
	{
	  CoglVertexBufferAttrib *attribute = tmp2->data;
	  CoglVertexBufferAttribFlags type =
	    attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_TYPE_MASK;

	  if (!(attribute->flags & COGL_VERTEX_BUFFER_ATTRIB_FLAG_ENABLED))
	    continue;

	  gl_type = get_gl_type_from_attribute_flags(attribute->flags);
	  switch (type)
	    {
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_COLOR_ARRAY:
	      /* GE (glDisableClientState (GL_COLOR_ARRAY)); */
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_NORMAL_ARRAY:
	      /* FIXME: go through cogl cache to enable normal array */
	      GE (glDisableClientState (GL_NORMAL_ARRAY));
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_TEXTURE_COORD_ARRAY:
              /* The enabled state of the texture coord arrays is
                 cached in ctx->enabled_texcoord_arrays so we don't
                 need to do anything here. The array will be disabled
                 by the next drawing primitive if it is not
                 required */
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_VERTEX_ARRAY:
	      /* GE (glDisableClientState (GL_VERTEX_ARRAY)); */
	      break;
	    case COGL_VERTEX_BUFFER_ATTRIB_FLAG_CUSTOM_ARRAY:
#ifdef MAY_HAVE_PROGRAMABLE_GL
	      /* FIXME: go through cogl cache to enable generic array */
	      GE (glDisableVertexAttribArray (generic_index++));
#endif
	      break;
	    default:
	      g_warning ("Unrecognised attribute type 0x%08x", type);
	    }
	}
    }
}

void
cogl_vertex_buffer_draw (CoglHandle       handle,
		         CoglVerticesMode mode,
		         int              first,
		         int              count)
{
  CoglVertexBuffer *buffer;
  CoglHandle source;

  if (!cogl_is_vertex_buffer (handle))
    return;

  _cogl_journal_flush ();

  buffer = _cogl_vertex_buffer_pointer_from_handle (handle);

  source = enable_state_for_drawing_buffer (buffer);

  GE (glDrawArrays (mode, first, count));

  disable_state_for_drawing_buffer (buffer, source);
}

static int
get_indices_type_size (GLuint indices_type)
{
  if (indices_type == GL_UNSIGNED_BYTE)
    return sizeof (GLubyte);
  else if (indices_type == GL_UNSIGNED_SHORT)
    return sizeof (GLushort);
  else if (indices_type == GL_UNSIGNED_INT)
    return sizeof (GLuint);
  else
    {
      g_critical ("Unknown indices type %d\n", indices_type);
      return 0;
    }
}

CoglHandle
cogl_vertex_buffer_indices_new (CoglIndicesType  indices_type,
                                const void      *indices_array,
                                int              indices_len)
{
  gboolean fallback =
    (cogl_get_features () & COGL_FEATURE_VBOS) ? FALSE : TRUE;
  gsize indices_bytes;
  CoglVertexBufferIndices *indices;

  _COGL_GET_CONTEXT (ctx, NULL);

  indices = g_slice_alloc (sizeof (CoglVertexBufferIndices));

  if (indices_type == COGL_INDICES_TYPE_UNSIGNED_BYTE)
    indices->type = GL_UNSIGNED_BYTE;
  else if (indices_type == COGL_INDICES_TYPE_UNSIGNED_SHORT)
    indices->type = GL_UNSIGNED_SHORT;
  else if (indices_type == COGL_INDICES_TYPE_UNSIGNED_INT)
    {
      g_return_val_if_fail (cogl_features_available
                            (COGL_FEATURE_UNSIGNED_INT_INDICES),
                            COGL_INVALID_HANDLE);

      indices->type = GL_UNSIGNED_INT;
    }
  else
    {
      g_critical ("unknown indices type %d", indices_type);
      g_slice_free (CoglVertexBufferIndices, indices);
      return NULL;
    }

  indices_bytes = get_indices_type_size (indices->type) * indices_len;
  if (fallback)
    {
      indices->vbo_name = g_malloc (indices_bytes);
      memcpy (indices->vbo_name, indices_array, indices_bytes);
    }
  else
    {
      GE (glGenBuffers (1, (GLuint *)&indices->vbo_name));
      GE (glBindBuffer (GL_ELEMENT_ARRAY_BUFFER,
			GPOINTER_TO_UINT (indices->vbo_name)));
      GE (glBufferData (GL_ELEMENT_ARRAY_BUFFER,
                        indices_bytes,
                        indices_array,
                        GL_STATIC_DRAW));
      GE (glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0));
    }

  return _cogl_vertex_buffer_indices_handle_new (indices);
}

CoglIndicesType
cogl_vertex_buffer_indices_get_type (CoglHandle indices_handle)
{
  CoglVertexBufferIndices *indices = NULL;

  if (!cogl_is_vertex_buffer_indices (indices_handle))
    return COGL_INDICES_TYPE_UNSIGNED_SHORT;

  indices = _cogl_vertex_buffer_indices_pointer_from_handle (indices_handle);

  if (indices->type == GL_UNSIGNED_BYTE)
    return COGL_INDICES_TYPE_UNSIGNED_BYTE;
  else if (indices->type == GL_UNSIGNED_SHORT)
    return COGL_INDICES_TYPE_UNSIGNED_SHORT;
  else
    {
      g_critical ("unknown indices type %d", indices->type);
      return COGL_INDICES_TYPE_UNSIGNED_SHORT;
    }
}

void
_cogl_vertex_buffer_indices_free (CoglVertexBufferIndices *indices)
{
  gboolean fallback =
    (cogl_get_features () & COGL_FEATURE_VBOS) ? FALSE : TRUE;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (fallback)
    g_free (indices->vbo_name);
  else
    GE (glDeleteBuffers (1, (GLuint *)&indices->vbo_name));

  g_slice_free (CoglVertexBufferIndices, indices);
}

void
cogl_vertex_buffer_draw_elements (CoglHandle       handle,
			          CoglVerticesMode mode,
                                  CoglHandle       indices_handle,
                                  int              min_index,
                                  int              max_index,
                                  int              indices_offset,
                                  int              count)
{
  CoglVertexBuffer *buffer;
  gboolean fallback =
    (cogl_get_features () & COGL_FEATURE_VBOS) ? FALSE : TRUE;
  gsize byte_offset;
  CoglVertexBufferIndices *indices = NULL;
  CoglHandle source;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (!cogl_is_vertex_buffer (handle))
    return;

  _cogl_journal_flush ();

  buffer = _cogl_vertex_buffer_pointer_from_handle (handle);

  if (!cogl_is_vertex_buffer_indices (indices_handle))
    return;

  indices = _cogl_vertex_buffer_indices_pointer_from_handle (indices_handle);

  source = enable_state_for_drawing_buffer (buffer);

  byte_offset = indices_offset * get_indices_type_size (indices->type);
  if (fallback)
    byte_offset = (size_t)(((char *)indices->vbo_name) + byte_offset);
  else
    GE (glBindBuffer (GL_ELEMENT_ARRAY_BUFFER,
                      GPOINTER_TO_UINT (indices->vbo_name)));

  GE (glDrawRangeElements (mode, min_index, max_index,
                           count, indices->type, (void *)byte_offset));

  disable_state_for_drawing_buffer (buffer, source);

  GE (glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0));
}

static void
_cogl_vertex_buffer_free (CoglVertexBuffer *buffer)
{
  GList *tmp;

  for (tmp = buffer->submitted_vbos; tmp != NULL; tmp = tmp->next)
    cogl_vertex_buffer_vbo_free (tmp->data, TRUE);
  g_list_free (buffer->submitted_vbos);

  for (tmp = buffer->new_attributes; tmp != NULL; tmp = tmp->next)
    cogl_vertex_buffer_attribute_free (tmp->data);
  g_list_free (buffer->new_attributes);

  g_slice_free (CoglVertexBuffer, buffer);
}

CoglHandle
cogl_vertex_buffer_indices_get_for_quads (unsigned int n_indices)
{
  _COGL_GET_CONTEXT (ctx, COGL_INVALID_HANDLE);

  /* Check if the indices would fit in a byte array */
  if (n_indices <= 256 / 4 * 6)
    {
      /* Generate the byte array if we haven't already */
      if (ctx->quad_indices_byte == COGL_INVALID_HANDLE)
        {
          guint8 *byte_array = g_malloc (256 / 4 * 6 * sizeof (guint8));
          guint8 *p = byte_array;
          int i, vert_num = 0;

          for (i = 0; i < 256 / 4; i++)
            {
              *(p++) = vert_num + 0;
              *(p++) = vert_num + 1;
              *(p++) = vert_num + 2;
              *(p++) = vert_num + 0;
              *(p++) = vert_num + 2;
              *(p++) = vert_num + 3;
              vert_num += 4;
            }

          ctx->quad_indices_byte
            = cogl_vertex_buffer_indices_new (COGL_INDICES_TYPE_UNSIGNED_BYTE,
                                              byte_array,
                                              256 / 4 * 6);

          g_free (byte_array);
        }

      return ctx->quad_indices_byte;
    }
  else
    {
      if (ctx->quad_indices_short_len < n_indices)
        {
          guint16 *short_array;
          guint16 *p;
          int i, vert_num = 0;

          if (ctx->quad_indices_short != COGL_INVALID_HANDLE)
            cogl_handle_unref (ctx->quad_indices_short);
          /* Pick a power of two >= MAX (512, n_indices) */
          if (ctx->quad_indices_short_len == 0)
            ctx->quad_indices_short_len = 512;
          while (ctx->quad_indices_short_len < n_indices)
            ctx->quad_indices_short_len *= 2;

          /* Over-allocate to generate a whole number of quads */
          p = short_array = g_malloc ((ctx->quad_indices_short_len
                                       + 5) / 6 * 6
                                      * sizeof (guint16));

          /* Fill in the complete quads */
          for (i = 0; i < ctx->quad_indices_short_len; i += 6)
            {
              *(p++) = vert_num + 0;
              *(p++) = vert_num + 1;
              *(p++) = vert_num + 2;
              *(p++) = vert_num + 0;
              *(p++) = vert_num + 2;
              *(p++) = vert_num + 3;
              vert_num += 4;
            }

          ctx->quad_indices_short
            = cogl_vertex_buffer_indices_new (COGL_INDICES_TYPE_UNSIGNED_SHORT,
                                              short_array,
                                              ctx->quad_indices_short_len);

          g_free (short_array);
        }

      return ctx->quad_indices_short;
    }
}
