/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2009 Intel Corp.
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
 * Author: Emmanuele Bassi <ebassi@linux.intel.com>
 */

/**
 * SECTION:clutter-input-device
 * @short_description: An input device managed by Clutter
 *
 * #ClutterInputDevice represents an input device known to Clutter.
 *
 * The #ClutterInputDevice class holds the state of the device, but
 * its contents are usually defined by the Clutter backend in use.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-debug.h"
#include "clutter-enum-types.h"
#include "clutter-input-device.h"
#include "clutter-private.h"

enum
{
  PROP_0,

  PROP_ID,
  PROP_DEVICE_TYPE,
  PROP_NAME
};

G_DEFINE_TYPE (ClutterInputDevice, clutter_input_device, G_TYPE_OBJECT);

static void
clutter_input_device_set_property (GObject      *gobject,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ClutterInputDevice *self = CLUTTER_INPUT_DEVICE (gobject);

  switch (prop_id)
    {
    case PROP_ID:
      self->id = g_value_get_int (value);
      break;

    case PROP_DEVICE_TYPE:
      self->device_type = g_value_get_enum (value);
      break;

    case PROP_NAME:
      self->device_name = g_strdup (g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_input_device_get_property (GObject    *gobject,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ClutterInputDevice *self = CLUTTER_INPUT_DEVICE (gobject);

  switch (prop_id)
    {
    case PROP_ID:
      g_value_set_int (value, self->id);
      break;

    case PROP_DEVICE_TYPE:
      g_value_set_enum (value, self->device_type);
      break;

    case PROP_NAME:
      g_value_set_string (value, self->device_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_input_device_class_init (ClutterInputDeviceClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  gobject_class->set_property = clutter_input_device_set_property;
  gobject_class->get_property = clutter_input_device_get_property;

  /**
   * ClutterInputDevice:id:
   *
   * The unique identifier of the device
   *
   * Since: 1.2
   */
  pspec = g_param_spec_int ("id",
                            P_("Id"),
                            P_("Unique identifier of the device"),
                            -1, G_MAXINT,
                            0,
                            CLUTTER_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (gobject_class, PROP_ID, pspec);

  /**
   * ClutterInputDevice:name:
   *
   * The name of the device
   *
   * Since: 1.2
   */
  pspec = g_param_spec_string ("name",
                               P_("Name"),
                               P_("The name of the device"),
                               NULL,
                               CLUTTER_PARAM_READWRITE |
                               G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (gobject_class, PROP_NAME, pspec);

  /**
   * ClutterInputDevice:device-type:
   *
   * The type of the device
   *
   * Since: 1.2
   */
  pspec = g_param_spec_enum ("device-type",
                             P_("Device Type"),
                             P_("The type of the device"),
                             CLUTTER_TYPE_INPUT_DEVICE_TYPE,
                             CLUTTER_POINTER_DEVICE,
                             CLUTTER_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY);
  g_object_class_install_property (gobject_class, PROP_DEVICE_TYPE, pspec);
}

static void
clutter_input_device_init (ClutterInputDevice *self)
{
  self->id = -1;
  self->device_type = CLUTTER_POINTER_DEVICE;

  self->click_count = 0;

  self->current_time = self->previous_time = CLUTTER_CURRENT_TIME;
  self->current_x = self->previous_x = -1;
  self->current_y = self->previous_y = -1;
  self->current_button_number = self->previous_button_number = -1;
  self->current_state = self->previous_state = 0;
}

/*
 * _clutter_input_device_set_coords:
 * @device: a #ClutterInputDevice
 * @x: X coordinate of the device
 * @y: Y coordinate of the device
 *
 * Stores the last known coordinates of the device
 */
void
_clutter_input_device_set_coords (ClutterInputDevice *device,
                                  gint                x,
                                  gint                y)
{
  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (device));

  if (device->current_x != x)
    device->current_x = x;

  if (device->current_y != y)
    device->current_y = y;
}

/*
 * _clutter_input_device_set_state:
 * @device: a #ClutterInputDevice
 * @state: a bitmask of modifiers
 *
 * Stores the last known modifiers state of the device
 */
void
_clutter_input_device_set_state (ClutterInputDevice  *device,
                                 ClutterModifierType  state)
{
  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (device));

  device->current_state = state;
}

/*
 * _clutter_input_device_set_time:
 * @device: a #ClutterInputDevice
 * @time_: the time
 *
 * Stores the last known event time of the device
 */
void
_clutter_input_device_set_time (ClutterInputDevice *device,
                                guint32             time_)
{
  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (device));

  if (device->current_time != time_)
    device->current_time = time_;
}

/*
 * cursor_weak_unref:
 *
 * #ClutterInputDevice keeps a weak reference on the actor
 * under its pointer; this function unsets the reference on
 * the actor to avoid keeping around stale pointers
 */
static void
cursor_weak_unref (gpointer  user_data,
                   GObject  *object_pointer)
{
  ClutterInputDevice *device = user_data;

  device->cursor_actor = NULL;
}

/*
 * _clutter_input_device_set_stage:
 * @device: a #ClutterInputDevice
 * @stage: a #ClutterStage or %NULL
 *
 * Stores the stage under the device
 */
void
_clutter_input_device_set_stage (ClutterInputDevice *device,
                                 ClutterStage       *stage)
{
  ClutterStage *old_stage;

  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (device));

  old_stage = device->stage;
  device->stage = stage;

  /* if we left the stage then we also need to unset the
   * cursor actor (and update its :has-pointer property)
   */
  if (device->stage == NULL &&
      device->cursor_actor != NULL &&
      device->cursor_actor != CLUTTER_ACTOR (old_stage))
    {
      ClutterEvent cev;

      cev.crossing.type = CLUTTER_LEAVE;
      cev.crossing.time = device->current_time;
      cev.crossing.flags = 0;
      cev.crossing.stage = old_stage;
      cev.crossing.source = device->cursor_actor;
      cev.crossing.x = device->current_x;
      cev.crossing.y = device->current_y;
      cev.crossing.device = device;
      cev.crossing.related = device->stage != NULL
                           ? CLUTTER_ACTOR (device->stage)
                           : CLUTTER_ACTOR (old_stage);

      _clutter_stage_queue_event (old_stage, &cev);

      _clutter_actor_set_has_pointer (device->cursor_actor, FALSE);
      g_object_weak_unref (G_OBJECT (device->cursor_actor),
                           cursor_weak_unref,
                           device);
    }

  device->cursor_actor = NULL;
}

/*
 * _clutter_input_device_set_actor:
 * @device: a #ClutterInputDevice
 * @actor: a #ClutterActor
 *
 * Sets the actor under the pointer coordinates of @device
 *
 * This function is called by _clutter_input_device_update()
 * and it will:
 *
 *   - queue a %CLUTTER_LEAVE event on the previous pointer actor
 *     of @device, if any
 *   - set to %FALSE the :has-pointer property of the previous
 *     pointer actor of @device, if any
 *   - queue a %CLUTTER_ENTER event on the new pointer actor
 *   - set to %TRUE the :has-pointer property of the new pointer
 *     actor
 */
void
_clutter_input_device_set_actor (ClutterInputDevice *device,
                                 ClutterActor       *actor)
{
  ClutterActor *old_actor;
  ClutterEvent cev;

  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (device));

  if (actor == device->cursor_actor)
    return;

  old_actor = device->cursor_actor;
  if (old_actor != NULL)
    {
      cev.crossing.type = CLUTTER_LEAVE;
      cev.crossing.time = device->current_time;
      cev.crossing.flags = 0;
      cev.crossing.stage = device->stage;
      cev.crossing.source = device->cursor_actor;
      cev.crossing.x = device->current_x;
      cev.crossing.y = device->current_y;
      cev.crossing.device = device;
      cev.crossing.related = actor;

      /* we need to make sure that this event is processed before
       * any other event we might have queued up until now, so we
       * go on and synthesize the event emission
       */
      _clutter_process_event (&cev);

      _clutter_actor_set_has_pointer (device->cursor_actor, FALSE);
      g_object_weak_unref (G_OBJECT (device->cursor_actor),
                           cursor_weak_unref,
                           device);

      device->cursor_actor = NULL;
    }

  if (actor != NULL)
    {
      cev.crossing.type = CLUTTER_ENTER;
      cev.crossing.time = device->current_time;
      cev.crossing.flags = 0;
      cev.crossing.stage = device->stage;
      cev.crossing.x = device->current_x;
      cev.crossing.y = device->current_y;
      cev.crossing.device = device;

      CLUTTER_NOTE (EVENT, "Device '%s' entering '%s' at %d, %d",
                    device->device_name,
                    clutter_actor_get_name (actor) != NULL
                      ? clutter_actor_get_name (actor)
                      : G_OBJECT_TYPE_NAME (actor),
                    device->current_x,
                    device->current_y);

      /* if there is an actor overlapping the Stage boundary and we
       * don't do this check then we'll emit an ENTER event only on
       * the actor instead of emitting it on the Stage *and* the
       * actor
       */
      if (old_actor == NULL && actor != CLUTTER_ACTOR (device->stage))
        {
          cev.crossing.source = CLUTTER_ACTOR (device->stage);
          cev.crossing.related = NULL;

          CLUTTER_NOTE (EVENT, "Adding Crossing[Enter] event for Stage");

          _clutter_process_event (&cev);

          cev.crossing.source = actor;
          cev.crossing.related = CLUTTER_ACTOR (device->stage);
        }
      else
        {
          cev.crossing.source = actor;
          cev.crossing.related = old_actor;
        }

      /* as above: we need to make sure that this event is processed
       * before any other event we might have queued up until now, so
       * we go on and synthesize the event emission
       */
      _clutter_process_event (&cev);
    }

  device->cursor_actor = actor;

  if (device->cursor_actor != NULL)
    {
      g_object_weak_ref (G_OBJECT (device->cursor_actor),
                         cursor_weak_unref,
                         device);
      _clutter_actor_set_has_pointer (device->cursor_actor, TRUE);
    }
}

/**
 * clutter_input_device_get_device_type:
 * @device: a #ClutterInputDevice
 *
 * Retrieves the type of @device
 *
 * Return value: the type of the device
 *
 * Since: 1.0
 */
ClutterInputDeviceType
clutter_input_device_get_device_type (ClutterInputDevice *device)
{
  g_return_val_if_fail (CLUTTER_IS_INPUT_DEVICE (device),
                        CLUTTER_POINTER_DEVICE);

  return device->device_type;
}

/**
 * clutter_input_device_get_device_id:
 * @device: a #ClutterInputDevice
 *
 * Retrieves the unique identifier of @device
 *
 * Return value: the identifier of the device
 *
 * Since: 1.0
 */
gint
clutter_input_device_get_device_id (ClutterInputDevice *device)
{
  g_return_val_if_fail (CLUTTER_IS_INPUT_DEVICE (device), -1);

  return device->id;
}

/**
 * clutter_input_device_get_device_coords:
 * @device: a #ClutterInputDevice of type %CLUTTER_POINTER_DEVICE
 * @x: (out): return location for the X coordinate
 * @y: (out): return location for the Y coordinate
 *
 * Retrieves the latest coordinates of the pointer of @device
 *
 * Since: 1.2
 */
void
clutter_input_device_get_device_coords (ClutterInputDevice *device,
                                        gint               *x,
                                        gint               *y)
{
  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (device));
  g_return_if_fail (device->device_type == CLUTTER_POINTER_DEVICE);

  if (x)
    *x = device->current_x;

  if (y)
    *y = device->current_y;
}

/*
 * _clutter_input_device_update:
 * @device: a #ClutterInputDevice
 *
 * Updates the input @device by determining the #ClutterActor underneath the
 * pointer's cursor
 *
 * This function calls _clutter_input_device_set_actor() if needed.
 *
 * This function only works for #ClutterInputDevice of type
 * %CLUTTER_POINTER_DEVICE.
 *
 * Since: 1.2
 */
ClutterActor *
_clutter_input_device_update (ClutterInputDevice *device)
{
  ClutterStage *stage;
  ClutterActor *new_cursor_actor;
  ClutterActor *old_cursor_actor;
  gint x, y;

  g_return_val_if_fail (device->device_type == CLUTTER_POINTER_DEVICE, NULL);

  stage = device->stage;
  if (stage == NULL)
    return NULL;

  clutter_input_device_get_device_coords (device, &x, &y);

  old_cursor_actor = device->cursor_actor;
  new_cursor_actor = _clutter_do_pick (stage, x, y, CLUTTER_PICK_REACTIVE);

  /* if the pick could not find an actor then we do not update the
   * input device, to avoid ghost enter/leave events; the pick should
   * never fail, except for bugs in the glReadPixels() implementation
   * in which case this is the safest course of action anyway
   */
  if (new_cursor_actor == NULL)
    return NULL;

  CLUTTER_NOTE (EVENT,
                "Actor under cursor (device %d, at %d, %d): %s",
                clutter_input_device_get_device_id (device),
                x, y,
                clutter_actor_get_name (new_cursor_actor) != NULL
                  ? clutter_actor_get_name (new_cursor_actor)
                  : G_OBJECT_TYPE_NAME (new_cursor_actor));

  /* short-circuit here */
  if (new_cursor_actor == old_cursor_actor)
    return old_cursor_actor;

  _clutter_input_device_set_actor (device, new_cursor_actor);

  return device->cursor_actor;
}

/**
 * clutter_input_device_get_pointer_actor:
 * @device: a #ClutterInputDevice of type %CLUTTER_POINTER_DEVICE
 *
 * Retrieves the #ClutterActor underneath the pointer of @device
 *
 * Return value: (transfer none): a pointer to the #ClutterActor or %NULL
 *
 * Since: 1.2
 */
ClutterActor *
clutter_input_device_get_pointer_actor (ClutterInputDevice *device)
{
  g_return_val_if_fail (CLUTTER_IS_INPUT_DEVICE (device), NULL);
  g_return_val_if_fail (device->device_type == CLUTTER_POINTER_DEVICE, NULL);

  return device->cursor_actor;
}

/**
 * clutter_input_device_get_pointer_stage:
 * @device: a #ClutterInputDevice of type %CLUTTER_POINTER_DEVICE
 *
 * Retrieves the #ClutterStage underneath the pointer of @device
 *
 * Return value: (transfer none): a pointer to the #ClutterStage or %NULL
 *
 * Since: 1.2
 */
ClutterStage *
clutter_input_device_get_pointer_stage (ClutterInputDevice *device)
{
  g_return_val_if_fail (CLUTTER_IS_INPUT_DEVICE (device), NULL);
  g_return_val_if_fail (device->device_type == CLUTTER_POINTER_DEVICE, NULL);

  return device->stage;
}

/**
 * clutter_input_device_get_device_name:
 * @device: a #ClutterInputDevice
 *
 * Retrieves the name of the @device
 *
 * Return value: the name of the device, or %NULL. The returned string
 *   is owned by the #ClutterInputDevice and should never be modified
 *   or freed
 *
 * Since: 1.2
 */
G_CONST_RETURN gchar *
clutter_input_device_get_device_name (ClutterInputDevice *device)
{
  g_return_val_if_fail (CLUTTER_IS_INPUT_DEVICE (device), NULL);

  return device->device_name;
}

/**
 * clutter_input_device_update_from_event:
 * @device: a #ClutterInputDevice
 * @event: a #ClutterEvent
 * @update_stage: whether to update the #ClutterStage of the @device
 *   using the stage of the event
 *
 * Forcibly updates the state of the @device using a #ClutterEvent
 *
 * This function should never be used by applications: it is meant
 * for integration with embedding toolkits, like clutter-gtk
 *
 * Embedding toolkits that disable the event collection inside Clutter
 * need to use this function to update the state of input devices depending
 * on a #ClutterEvent that they are going to submit to the event handling code
 * in Clutter though clutter_do_event(). Since the input devices hold the state
 * that is going to be used to fill in fields like the #ClutterButtonEvent
 * click count, or to emit synthesized events like %CLUTTER_ENTER and
 * %CLUTTER_LEAVE, it is necessary for embedding toolkits to also be
 * responsible of updating the input device state.
 *
 * For instance, this might be the code to translate an embedding toolkit
 * native motion notification into a Clutter #ClutterMotionEvent and ask
 * Clutter to process it:
 *
 * |[
 *   ClutterEvent c_event;
 *
 *   translate_native_event_to_clutter (native_event, &amp;c_event);
 *
 *   clutter_do_event (&amp;c_event);
 * ]|
 *
 * Before letting clutter_do_event() process the event, it is necessary to call
 * clutter_input_device_update_from_event():
 *
 * |[
 *   ClutterEvent c_event;
 *   ClutterDeviceManager *manager;
 *   ClutterInputDevice *device;
 *
 *   translate_native_event_to_clutter (native_event, &amp;c_event);
 *
 *   /&ast; get the device manager &ast;/
 *   manager = clutter_device_manager_get_default ();
 *
 *   /&ast; use the default Core Pointer that Clutter
 *    &ast; backends register by default
 *    &ast;/
 *   device = clutter_device_manager_get_core_device (manager, %CLUTTER_POINTER_DEVICE);
 *
 *   /&ast; update the state of the input device &ast;/
 *   clutter_input_device_update_from_event (device, &amp;c_event, FALSE);
 *
 *   clutter_do_event (&amp;c_event);
 * ]|
 *
 * The @update_stage boolean argument should be used when the input device
 * enters and leaves a #ClutterStage; it will use the #ClutterStage field
 * of the passed @event to update the stage associated to the input device.
 *
 * Since: 1.2
 */
void
clutter_input_device_update_from_event (ClutterInputDevice *device,
                                        ClutterEvent       *event,
                                        gboolean            update_stage)
{
  ClutterModifierType event_state;
  ClutterStage *event_stage;
  gfloat event_x, event_y;
  guint32 event_time;

  g_return_if_fail (CLUTTER_IS_INPUT_DEVICE (device));
  g_return_if_fail (event != NULL);

  event_state = clutter_event_get_state (event);
  event_time = clutter_event_get_time (event);
  event_stage = clutter_event_get_stage (event);
  clutter_event_get_coords (event, &event_x, &event_y);

  _clutter_input_device_set_coords (device, event_x, event_y);
  _clutter_input_device_set_state (device, event_state);
  _clutter_input_device_set_time (device, event_time);

  if (update_stage)
    _clutter_input_device_set_stage (device, event_stage);
}
