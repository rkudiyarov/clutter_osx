/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2008  Intel Corporation.
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
 * Author:
 *   Emmanuele Bassi <ebassi@linux.intel.com>
 */

/**
 * SECTION:clutter-animation
 * @short_description: Simple implicit animations
 *
 * #ClutterAnimation is an object providing simple, implicit animations
 * for #ClutterActor<!-- -->s.
 *
 * #ClutterAnimation instances will bind a #GObject property belonging
 * to a #ClutterActor to a #ClutterInterval, and will then use a
 * #ClutterTimeline to interpolate the property between the initial
 * and final values of the interval.
 *
 * For convenience, it is possible to use the clutter_actor_animate()
 * function call which will take care of setting up and tearing down
 * a #ClutterAnimation instance and animate an actor between its current
 * state and the specified final state.
 *
 * #ClutterAnimation is available since Clutter 1.0
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib-object.h>
#include <gobject/gvaluecollector.h>

#include "clutter-alpha.h"
#include "clutter-animation.h"
#include "clutter-debug.h"
#include "clutter-enum-types.h"
#include "clutter-interval.h"
#include "clutter-private.h"

enum
{
  PROP_0,

  PROP_ACTOR,
  PROP_MODE,
  PROP_DURATION,
  PROP_LOOP,
  PROP_TIMELINE,
  PROP_ALPHA
};

enum
{
  COMPLETED,

  LAST_SIGNAL
};

#define CLUTTER_ANIMATION_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), CLUTTER_TYPE_ANIMATION, ClutterAnimationPrivate))

struct _ClutterAnimationPrivate
{
  ClutterActor *actor;

  GHashTable *properties;

  ClutterAnimationMode mode;

  guint loop : 1;
  guint duration;
  ClutterTimeline *timeline;
  guint timeline_completed_id;

  ClutterAlpha *alpha;
  guint alpha_notify_id;
};

static guint animation_signals[LAST_SIGNAL] = { 0, };

static GQuark quark_actor_animation = 0;

G_DEFINE_TYPE (ClutterAnimation, clutter_animation, G_TYPE_INITIALLY_UNOWNED);

static void on_animation_weak_notify (gpointer  data,
                                      GObject  *animation_pointer);

static void
clutter_animation_finalize (GObject *gobject)
{
  ClutterAnimationPrivate *priv = CLUTTER_ANIMATION (gobject)->priv;

  g_hash_table_destroy (priv->properties);

  G_OBJECT_CLASS (clutter_animation_parent_class)->finalize (gobject);
}

static void
clutter_animation_dispose (GObject *gobject)
{
  ClutterAnimationPrivate *priv = CLUTTER_ANIMATION (gobject)->priv;

  if (priv->actor)
    {
      g_object_weak_unref (G_OBJECT (gobject),
                           on_animation_weak_notify,
                           priv->actor);
      g_object_set_qdata (G_OBJECT (priv->actor),
                          quark_actor_animation,
                          NULL);
      g_object_unref (priv->actor);
      priv->actor = NULL;
    }

  if (priv->timeline)
    {
      if (priv->timeline_completed_id)
        {
          g_signal_handler_disconnect (priv->timeline,
                                       priv->timeline_completed_id);
          priv->timeline_completed_id = 0;
        }

      g_object_unref (priv->timeline);
      priv->timeline = NULL;
    }

  if (priv->alpha)
    {
      if (priv->alpha_notify_id)
        {
          g_signal_handler_disconnect (priv->alpha, priv->alpha_notify_id);
          priv->alpha_notify_id = 0;
        }

      g_object_unref (priv->alpha);
      priv->alpha = NULL;
    }

  G_OBJECT_CLASS (clutter_animation_parent_class)->dispose (gobject);
}

static void
clutter_animation_set_property (GObject      *gobject,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ClutterAnimation *animation = CLUTTER_ANIMATION (gobject);

  switch (prop_id)
    {
    case PROP_ACTOR:
      clutter_animation_set_actor (animation, g_value_get_object (value));
      break;

    case PROP_MODE:
      clutter_animation_set_mode (animation, g_value_get_enum (value));
      break;

    case PROP_DURATION:
      clutter_animation_set_duration (animation, g_value_get_uint (value));
      break;

    case PROP_LOOP:
      clutter_animation_set_loop (animation, g_value_get_boolean (value));
      break;

    case PROP_TIMELINE:
      clutter_animation_set_timeline (animation, g_value_get_object (value));
      break;

    case PROP_ALPHA:
      clutter_animation_set_alpha (animation, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_animation_get_property (GObject    *gobject,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ClutterAnimationPrivate *priv = CLUTTER_ANIMATION (gobject)->priv;

  switch (prop_id)
    {
    case PROP_ACTOR:
      g_value_set_object (value, priv->actor);
      break;

    case PROP_MODE:
      g_value_set_enum (value, priv->mode);
      break;

    case PROP_DURATION:
      g_value_set_uint (value, priv->duration);
      break;

    case PROP_LOOP:
      g_value_set_boolean (value, priv->loop);
      break;

    case PROP_TIMELINE:
      g_value_set_object (value, priv->timeline);
      break;

    case PROP_ALPHA:
      g_value_set_object (value, priv->alpha);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_animation_real_completed (ClutterAnimation *animation)
{
  CLUTTER_NOTE (ANIMATION, "Animation [%p] complete: unreffing",
                animation);

  g_object_unref (animation);
}

static void
clutter_animation_class_init (ClutterAnimationClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  quark_actor_animation =
    g_quark_from_static_string ("clutter-actor-animation");

  g_type_class_add_private (klass, sizeof (ClutterAnimationPrivate));

  klass->completed = clutter_animation_real_completed;

  gobject_class->set_property = clutter_animation_set_property;
  gobject_class->get_property = clutter_animation_get_property;
  gobject_class->dispose = clutter_animation_dispose;
  gobject_class->finalize = clutter_animation_finalize;

  /**
   * ClutterAnimation:actor:
   *
   * The actor to which the animation applies.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_object ("actor",
                               "Actor",
                               "Actor to which the animation applies",
                               CLUTTER_TYPE_ACTOR,
                               CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_ACTOR, pspec);

  /**
   * ClutterAnimation:mode:
   *
   * The animation mode.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_enum ("mode",
                             "Mode",
                             "The mode of the animation",
                             CLUTTER_TYPE_ANIMATION_MODE,
                             CLUTTER_LINEAR,
                             CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_MODE, pspec);

  /**
   * ClutterAnimation:duration:
   *
   * The duration of the animation, expressed in milliseconds.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_uint ("duration",
                             "Duration",
                             "Duration of the animation, in milliseconds",
                             0, G_MAXUINT, 0,
                             CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_DURATION, pspec);

  /**
   * ClutterAnimation:loop:
   *
   * Whether the animation should loop.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_boolean ("loop",
                                "Loop",
                                "Whether the animation should loop",
                                FALSE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_LOOP, pspec);

  /**
   * ClutterAnimation:timeline:
   *
   * The #ClutterTimeline used by the animation.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_object ("timeline",
                               "Timeline",
                               "The timeline used by the animation",
                               CLUTTER_TYPE_TIMELINE,
                               CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_TIMELINE, pspec);

  /**
   * ClutterAnimation:alpha:
   *
   * The #ClutterAlpha used by the animation.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_object ("alpha",
                               "Alpha",
                               "The alpha used by the animation",
                               CLUTTER_TYPE_ALPHA,
                               CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_ALPHA, pspec);

  /**
   * ClutterAniamtion::completed:
   * @animation: the animation that emitted the signal
   *
   * The ::completed signal is emitted once the animation has
   * been completed.
   *
   * Since: 1.0
   */
  animation_signals[COMPLETED] =
    g_signal_new (I_("completed"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ClutterAnimationClass, completed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

static void
clutter_animation_init (ClutterAnimation *self)
{
  self->priv = CLUTTER_ANIMATION_GET_PRIVATE (self);

  self->priv->mode = CLUTTER_LINEAR;
  self->priv->properties =
    g_hash_table_new_full (g_str_hash, g_str_equal,
                           (GDestroyNotify) g_free,
                           (GDestroyNotify) g_object_unref);
}

static inline void
clutter_animation_bind_property_internal (ClutterAnimation *animation,
                                          GParamSpec       *pspec,
                                          ClutterInterval  *interval)
{
  ClutterAnimationPrivate *priv = animation->priv;

  if (!clutter_interval_validate (interval, pspec))
    {
      g_warning ("Cannot bind property `%s': the interval is out "
                 "of bounds",
                 pspec->name);
      return;
    }

  g_hash_table_insert (priv->properties,
                       g_strdup (pspec->name),
                       g_object_ref_sink (interval));
}

static inline void
clutter_animation_update_property_internal (ClutterAnimation *animation,
                                            GParamSpec       *pspec,
                                            ClutterInterval  *interval)
{
  ClutterAnimationPrivate *priv = animation->priv;

  if (!clutter_interval_validate (interval, pspec))
    {
      g_warning ("Cannot bind property `%s': the interval is out "
                 "of bounds",
                 pspec->name);
      return;
    }

  g_hash_table_replace (priv->properties,
                        g_strdup (pspec->name),
                        g_object_ref_sink (interval));
}

/**
 * clutter_animation_bind_property:
 * @animation: a #ClutterAnimation
 * @property_name: the property to control
 * @interval: a #ClutterInterval
 *
 * Binds @interval to the @property_name of the #ClutterActor
 * attached to @animation. The #ClutterAnimation will take
 * ownership of the passed #ClutterInterval.
 *
 * If you need to update the interval instance use
 * clutter_animation_update_property() instead.
 *
 * Since: 1.0
 */
void
clutter_animation_bind_property (ClutterAnimation *animation,
                                 const gchar      *property_name,
                                 ClutterInterval  *interval)
{
  ClutterAnimationPrivate *priv;
  GObjectClass *klass;
  GParamSpec *pspec;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));
  g_return_if_fail (property_name != NULL);
  g_return_if_fail (CLUTTER_IS_INTERVAL (interval));

  priv = animation->priv;

  if (G_UNLIKELY (!priv->actor))
    {
      g_warning ("Cannot bind property `%s': the animation has no "
                 "actor set. You need to call clutter_animation_set_actor() "
                 "first to be able to bind a property",
                 property_name);
      return;
    }

  if (G_UNLIKELY (clutter_animation_has_property (animation, property_name)))
    {
      g_warning ("Cannot bind property `%s': the animation already has "
                 "a bound property with the same name",
                 property_name);
      return;
    }

  klass = G_OBJECT_GET_CLASS (priv->actor);
  pspec = g_object_class_find_property (klass, property_name);
  if (!pspec)
    {
      g_warning ("Cannot bind property `%s': actors of type `%s' have "
                 "no such property",
                 property_name,
                 g_type_name (G_OBJECT_TYPE (priv->actor)));
      return;
    }

  if (!(pspec->flags & G_PARAM_WRITABLE))
    {
      g_warning ("Cannot bind property `%s': the property is not writable",
                 property_name);
      return;
    }

  if (!g_value_type_compatible (G_PARAM_SPEC_VALUE_TYPE (pspec),
                                clutter_interval_get_value_type (interval)))
    {
      g_warning ("Cannot bind property `%s': the interval value of "
                 "type `%s' is not compatible with the property value "
                 "of type `%s'",
                 property_name,
                 g_type_name (clutter_interval_get_value_type (interval)),
                 g_type_name (G_PARAM_SPEC_VALUE_TYPE (pspec)));
      return;
    }

  clutter_animation_bind_property_internal (animation, pspec, interval);
}

/**
 * clutter_animation_unbind_property:
 * @animation: a #ClutterAnimation
 * @property_name: name of the property
 *
 * Removes @property_name from the list of animated properties.
 *
 * Since: 1.0
 */
void
clutter_animation_unbind_property (ClutterAnimation *animation,
                                   const gchar      *property_name)
{
  ClutterAnimationPrivate *priv;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));
  g_return_if_fail (property_name != NULL);

  priv = animation->priv;

  if (!clutter_animation_has_property (animation, property_name))
    {
      g_warning ("Cannot unbind property `%s': the animation has "
                 "no bound property with that name",
                 property_name);
      return;
    }

  g_hash_table_remove (priv->properties, property_name);
}

/**
 * clutter_animation_has_property:
 * @animation: a #ClutterAnimation
 * @property_name: name of the property
 *
 * Checks whether @animation is controlling @property_name.
 *
 * Return value: %TRUE if the property is animated by the
 *   #ClutterAnimation, %FALSE otherwise
 *
 * Since: 1.0
 */
gboolean
clutter_animation_has_property (ClutterAnimation *animation,
                                const gchar      *property_name)
{
  ClutterAnimationPrivate *priv;

  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), FALSE);
  g_return_val_if_fail (property_name != NULL, FALSE);

  priv = animation->priv;

  return g_hash_table_lookup (priv->properties, property_name) != NULL;
}

/**
 * clutter_animation_update_property:
 * @animation: a #ClutterAnimation
 * @property_name: name of the property
 * @interval: a #ClutterInterval
 *
 * Changes the @interval for @property_name. The #ClutterAnimation
 * will take ownership of the passed #ClutterInterval.
 *
 * Since: 1.0
 */
void
clutter_animation_update_property (ClutterAnimation *animation,
                                   const gchar      *property_name,
                                   ClutterInterval  *interval)
{
  ClutterAnimationPrivate *priv;
  GObjectClass *klass;
  GParamSpec *pspec;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));
  g_return_if_fail (property_name != NULL);
  g_return_if_fail (CLUTTER_IS_INTERVAL (interval));

  priv = animation->priv;

  if (!clutter_animation_has_property (animation, property_name))
    {
      g_warning ("Cannot unbind property `%s': the animation has "
                 "no bound property with that name",
                 property_name);
      return;
    }

  klass = G_OBJECT_GET_CLASS (priv->actor);
  pspec = g_object_class_find_property (klass, property_name);
  if (!pspec)
    {
      g_warning ("Cannot bind property `%s': actors of type `%s' have "
                 "no such property",
                 property_name,
                 g_type_name (G_OBJECT_TYPE (priv->actor)));
      return;
    }

  if (!g_value_type_compatible (G_PARAM_SPEC_VALUE_TYPE (pspec),
                                clutter_interval_get_value_type (interval)))
    {
      g_warning ("Cannot bind property `%s': the interval value of "
                 "type `%s' is not compatible with the property value "
                 "of type `%s'",
                 property_name,
                 g_type_name (clutter_interval_get_value_type (interval)),
                 g_type_name (G_PARAM_SPEC_VALUE_TYPE (pspec)));
      return;
    }

  clutter_animation_update_property_internal (animation, pspec, interval);
}

/**
 * clutter_animation_get_interval:
 * @animation: a #ClutterAnimation
 * @property_name: name of the property
 *
 * Retrieves the #ClutterInterval associated to @property_name
 * inside @animation.
 *
 * Return value: a #ClutterInterval or %NULL if no property with
 *   the same name was found. The returned interval is owned by
 *   the #ClutterAnimation and should not be unreferenced
 *
 * Since: 1.0
 */
ClutterInterval *
clutter_animation_get_interval (ClutterAnimation *animation,
                                const gchar      *property_name)
{
  ClutterAnimationPrivate *priv;

  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), NULL);
  g_return_val_if_fail (property_name != NULL, NULL);

  priv = animation->priv;

  return g_hash_table_lookup (priv->properties, property_name);
}

static void
on_timeline_completed (ClutterTimeline  *timeline,
                       ClutterAnimation *animation)
{
  CLUTTER_NOTE (ANIMATION, "Timeline [%p] complete", timeline);

  if (!animation->priv->loop)
    g_signal_emit (animation, animation_signals[COMPLETED], 0);
}

static void
on_alpha_notify (GObject          *gobject,
                 GParamSpec       *pspec,
                 ClutterAnimation *animation)
{
  ClutterAnimationPrivate *priv = animation->priv;
  GList *properties, *p;
  guint32 alpha_value;

  alpha_value = clutter_alpha_get_alpha (CLUTTER_ALPHA (gobject));

  g_object_freeze_notify (G_OBJECT (priv->actor));

  properties = g_hash_table_get_keys (priv->properties);
  for (p = properties; p != NULL; p = p->next)
    {
      const gchar *p_name = p->data;
      ClutterInterval *interval;
      gdouble factor;
      GValue value = { 0, };

      interval = g_hash_table_lookup (priv->properties, p_name);
      g_assert (CLUTTER_IS_INTERVAL (interval));

      g_value_init (&value, clutter_interval_get_value_type (interval));

      factor = (gdouble) alpha_value / CLUTTER_ALPHA_MAX_ALPHA;
      clutter_interval_compute_value (interval, factor, &value);

      g_object_set_property (G_OBJECT (priv->actor), p_name, &value);

      g_value_unset (&value);
    }

  g_list_free (properties);

  g_object_thaw_notify (G_OBJECT (priv->actor));
}

/*
 * Removes the animation pointer from the qdata section of the
 * actor attached to the animation
 */
static void
on_animation_weak_notify (gpointer  data,
                          GObject  *animation_pointer)
{
  GObject *actor = data;

  CLUTTER_NOTE (ANIMATION, "Removing Animation from actor %d[%p]",
                clutter_actor_get_gid (CLUTTER_ACTOR (actor)),
                actor);

  g_object_set_qdata (actor, quark_actor_animation, NULL);
}

ClutterAnimation *
clutter_animation_new (void)
{
  return g_object_new (CLUTTER_TYPE_ANIMATION, NULL);
}

/**
 * clutter_animation_set_actor:
 * @animation: a #ClutterAnimation
 * @actor: a #ClutterActor
 *
 * Attaches @animation to @actor. The #ClutterAnimation will take a
 * reference on @actor.
 *
 * Since: 1.0
 */
void
clutter_animation_set_actor (ClutterAnimation *animation,
                             ClutterActor     *actor)
{
  ClutterAnimationPrivate *priv;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  priv = animation->priv;

  if (priv->actor)
    {
      g_object_weak_unref (G_OBJECT (animation),
                           on_animation_weak_notify,
                           priv->actor);
      g_object_set_qdata (G_OBJECT (priv->actor),
                          quark_actor_animation,
                          NULL);
      g_object_unref (priv->actor);
    }

  priv->actor = g_object_ref (actor);
  g_object_weak_ref (G_OBJECT (animation),
                     on_animation_weak_notify,
                     priv->actor);
  g_object_set_qdata (G_OBJECT (priv->actor),
                      quark_actor_animation,
                      animation);

  g_object_notify (G_OBJECT (animation), "actor");
}

/**
 * clutter_animation_get_actor:
 * @animation: a #ClutterAnimation
 *
 * Retrieves the #ClutterActor attached to @animation.
 *
 * Return value: a #ClutterActor
 *
 * Since: 1.0
 */
ClutterActor *
clutter_animation_get_actor (ClutterAnimation *animation)
{
  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), NULL);

  return animation->priv->actor;
}

static inline void
clutter_animation_set_mode_internal (ClutterAnimation *animation)
{
  ClutterAnimationPrivate *priv = animation->priv;
  ClutterAlpha *alpha;

  alpha = clutter_animation_get_alpha (animation);
  if (alpha)
    clutter_alpha_set_mode (alpha, priv->mode);
}

/**
 * clutter_animation_set_mode:
 * @animation: a #ClutterAnimation
 * @mode: a #ClutterAnimationMode
 *
 * Sets the animation @mode of @animation.
 *
 * Since: 1.0
 */
void
clutter_animation_set_mode (ClutterAnimation     *animation,
                            ClutterAnimationMode  mode)
{
  ClutterAnimationPrivate *priv;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));

  priv = animation->priv;

  priv->mode = mode;
  clutter_animation_set_mode_internal (animation);

  g_object_notify (G_OBJECT (animation), "mode");
}

/**
 * clutter_animation_get_mode:
 * @animation: a #ClutterAnimation
 *
 * Retrieves the animation mode of @animation.
 *
 * Return value: the #ClutterAnimationMode for the animation
 *
 * Since: 1.0
 */
ClutterAnimationMode
clutter_animation_get_mode (ClutterAnimation *animation)
{
  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), CLUTTER_LINEAR);

  return animation->priv->mode;
}

/**
 * clutter_animation_set_duration:
 * @animation: a #ClutterAnimation
 * @msecs: the duration in milliseconds
 *
 * Sets the duration of @animation in milliseconds.
 *
 * Since: 1.0
 */
void
clutter_animation_set_duration (ClutterAnimation *animation,
                                gint              msecs)
{
  ClutterAnimationPrivate *priv;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));

  priv = animation->priv;

  priv->duration = msecs;

  if (priv->timeline)
    {
      gboolean was_playing;

      was_playing = clutter_timeline_is_playing (priv->timeline);
      if (was_playing)
        clutter_timeline_stop (priv->timeline);

      clutter_timeline_set_duration (priv->timeline, msecs);

      if (was_playing)
        clutter_timeline_start (priv->timeline);
    }

  g_object_notify (G_OBJECT (animation), "duration");
}

/**
 * clutter_animation_set_loop:
 * @animation: a #ClutterAnimation
 * @loop: %TRUE if the animation should loop
 *
 * Sets whether @animation should loop over itself once finished.
 *
 * A looping #ClutterAnimation will not emit the #ClutterAnimation::completed
 * signal when finished.
 *
 * Since: 1.0
 */
void
clutter_animation_set_loop (ClutterAnimation *animation,
                            gboolean          loop)
{
  ClutterAnimationPrivate *priv;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));

  priv = animation->priv;

  if (priv->loop != loop)
    {
      priv->loop = loop;

      if (priv->timeline)
        clutter_timeline_set_loop (priv->timeline, priv->loop);

      g_object_notify (G_OBJECT (animation), "loop");
    }
}

/**
 * clutter_animation_get_loop:
 * @animation: a #ClutterAnimation
 *
 * Retrieves whether @animation is looping.
 *
 * Return value: %TRUE if the animation is looping
 *
 * Since: 1.0
 */
gboolean
clutter_animation_get_loop (ClutterAnimation *animation)
{
  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), FALSE);

  return animation->priv->loop;
}

/**
 * clutter_animation_get_duration:
 * @animation: a #ClutterAnimation
 *
 * Retrieves the duration of @animation, in milliseconds.
 *
 * Return value: the duration of the animation
 *
 * Since: 1.0
 */
guint
clutter_animation_get_duration (ClutterAnimation *animation)
{
  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), 0);

  return animation->priv->duration;
}

/**
 * clutter_animation_set_timeline:
 * @animation: a #ClutterAnimation
 * @timeline: a #ClutterTimeline or %NULL
 *
 * Sets the #ClutterTimeline used by @animation.
 *
 * The #ClutterAnimation:duration and #ClutterAnimation:loop properties
 * will be set using the corresponding #ClutterTimeline properties as a
 * side effect.
 *
 * If @timeline is %NULL a new #ClutterTimeline will be constructed
 * using the current values of the #ClutterAnimation:duration and
 * #ClutterAnimation:loop properties.
 *
 * Since: 1.0
 */
void
clutter_animation_set_timeline (ClutterAnimation *animation,
                                ClutterTimeline  *timeline)
{
  ClutterAnimationPrivate *priv;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));
  g_return_if_fail (timeline == NULL || CLUTTER_IS_TIMELINE (timeline));

  priv = animation->priv;

  g_object_freeze_notify (G_OBJECT (animation));

  if (priv->timeline)
    {
      if (priv->timeline_completed_id)
        g_signal_handler_disconnect (priv->timeline,
                                     priv->timeline_completed_id);

      g_object_unref (priv->timeline);
      priv->timeline_completed_id = 0;
      priv->timeline = 0;
    }

  if (!timeline)
    timeline = g_object_new (CLUTTER_TYPE_TIMELINE,
                             "duration", priv->duration,
                             "loop", priv->loop,
                             NULL);
  else
    {
      priv->duration = clutter_timeline_get_duration (timeline);
      g_object_notify (G_OBJECT (animation), "duration");

      priv->loop = clutter_timeline_get_loop (timeline);
      g_object_notify (G_OBJECT (animation), "loop");
    }

  priv->timeline = g_object_ref (timeline);
  g_object_notify (G_OBJECT (animation), "timeline");

  priv->timeline_completed_id =
    g_signal_connect (timeline, "completed",
                      G_CALLBACK (on_timeline_completed),
                      animation);

  g_object_thaw_notify (G_OBJECT (animation));
}

/**
 * clutter_animation_get_timeline:
 * @animation: a #ClutterAnimation
 *
 * Retrieves the #ClutterTimeline used by @animation
 *
 * Return value: the timeline used by the animation
 *
 * Since: 1.0
 */
ClutterTimeline *
clutter_animation_get_timeline (ClutterAnimation *animation)
{
  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), NULL);

  return animation->priv->timeline;
}

/**
 * clutter_animation_set_alpha:
 * @animation: a #ClutterAnimation
 * @alpha: a #ClutterAlpha, or %NULL
 *
 * Sets @alpha as the #ClutterAlpha used by @animation.
 *
 * If @alpha is %NULL, a new #ClutterAlpha will be constructed from
 * the current values of the #ClutterAnimation:mode and
 * #ClutterAnimation:timeline properties.
 *
 * Since: 1.0
 */
void
clutter_animation_set_alpha (ClutterAnimation *animation,
                             ClutterAlpha     *alpha)
{
  ClutterAnimationPrivate *priv;

  g_return_if_fail (CLUTTER_IS_ANIMATION (animation));
  g_return_if_fail (alpha == NULL || CLUTTER_IS_ALPHA (alpha));

  priv = animation->priv;

  if (priv->alpha)
    {
      if (priv->alpha_notify_id)
        g_signal_handler_disconnect (priv->alpha, priv->alpha_notify_id);

      g_object_unref (priv->alpha);
      priv->alpha_notify_id = 0;
      priv->alpha = NULL;
    }

  if (!alpha)
    {
      ClutterTimeline *timeline;

      timeline = clutter_animation_get_timeline (animation);

      alpha = clutter_alpha_new ();
      clutter_alpha_set_timeline (alpha, timeline);
      clutter_animation_set_mode_internal (animation);
    }

  priv->alpha = g_object_ref_sink (alpha);

  priv->alpha_notify_id =
    g_signal_connect (alpha, "notify::alpha",
                      G_CALLBACK (on_alpha_notify),
                      animation);
}

/**
 * clutter_animation_get_alpha:
 * @animation: a #ClutterAnimation
 *
 * Retrieves the #ClutterAlpha used by @animation.
 *
 * Return value: the alpha object used by the animation
 *
 * Since: 1.0
 */
ClutterAlpha *
clutter_animation_get_alpha (ClutterAnimation *animation)
{
  g_return_val_if_fail (CLUTTER_IS_ANIMATION (animation), NULL);

  return animation->priv->alpha;
}

/*
 * starts the timeline
 */
static void
clutter_animation_start (ClutterAnimation *animation)
{
  if (animation->priv->timeline)
    clutter_timeline_start (animation->priv->timeline);
  else
    {
      /* sanity check */
      g_warning (G_STRLOC ": no timeline found, unable to start the animation");
    }
}

static inline void
clutter_animation_setup_valist (ClutterAnimation *animation,
                                const gchar      *first_property_name,
                                va_list           var_args)
{
  ClutterAnimationPrivate *priv = animation->priv;
  GObjectClass *klass;
  const gchar *property_name;

  klass = G_OBJECT_GET_CLASS (priv->actor);

  property_name = first_property_name;
  while (property_name != NULL)
    {
      GValue final = { 0, };
      GParamSpec *pspec = NULL;
      gboolean is_fixed = FALSE;
      gchar *error = NULL;

      /* fixed properties will not be animated */
      if (g_str_has_prefix (property_name, "fixed::"))
        {
          is_fixed = TRUE;
          property_name += 7;
        }

      pspec = g_object_class_find_property (klass, property_name);
      if (!pspec)
        {
          g_warning ("Cannot bind property `%s': actors of type `%s' do "
                     "not have this property",
                     property_name,
                     g_type_name (G_OBJECT_TYPE (priv->actor)));
          break;
        }

      if (!(pspec->flags & G_PARAM_WRITABLE))
        {
          g_warning ("Cannot bind property `%s': the property is "
                     "not writable",
                     property_name);
          break;
        }

      g_value_init (&final, G_PARAM_SPEC_VALUE_TYPE (pspec));
      G_VALUE_COLLECT (&final, var_args, 0, &error);
      if (error)
        {
          g_warning ("%s: %s", G_STRLOC, error);
          g_free (error);
          break;
        }

      /* create an interval and bind it to the property, in case
       * it's not a fixed property, otherwise just set it
       */
      if (G_LIKELY (!is_fixed))
        {
          ClutterInterval *interval;
          GValue initial = { 0, };

          g_value_init (&initial, G_PARAM_SPEC_VALUE_TYPE (pspec));
          g_object_get_property (G_OBJECT (priv->actor),
                                 property_name,
                                 &initial);

          interval =
            clutter_interval_new_with_values (G_PARAM_SPEC_VALUE_TYPE (pspec),
                                              &initial,
                                              &final);

          if (!clutter_animation_has_property (animation, pspec->name))
            clutter_animation_bind_property_internal (animation,
                                                      pspec,
                                                      interval);
          else
            clutter_animation_update_property_internal (animation,
                                                        pspec,
                                                        interval);

          g_value_unset (&initial);
        }
      else
        g_object_set_property (G_OBJECT (priv->actor), property_name, &final);

      g_value_unset (&final);

      property_name = va_arg (var_args, gchar*);
    }

  /* start the animation by default */
  clutter_animation_start (animation);
}

/**
 * clutter_actor_animate_with_alpha:
 * @actor: a #ClutterActor
 * @alpha: a #ClutterAlpha
 * @first_property_name: the name of a property
 * @VarArgs: a %NULL terminated list of property names and
 *   property values
 *
 * Animates the given list of properties of @actor between the current
 * value for each property and a new final value. The animation has a
 * definite behaviour given by the passed @alpha.
 *
 * See clutter_actor_animate() for further details.
 *
 * This function is useful if you want to use an existing #ClutterAlpha
 * to animate @actor.
 *
 * Return value: a #ClutterAnimation object. The object is owned by the
 *   #ClutterActor and should not be unreferenced with g_object_unref()
 *
 * Since: 1.0
 */
ClutterAnimation *
clutter_actor_animate_with_alpha (ClutterActor *actor,
                                  ClutterAlpha *alpha,
                                  const gchar  *first_property_name,
                                  ...)
{
  ClutterAnimation *animation;
  ClutterTimeline *timeline;
  va_list args;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);
  g_return_val_if_fail (CLUTTER_IS_ALPHA (alpha), NULL);
  g_return_val_if_fail (first_property_name != NULL, NULL);

  timeline = clutter_alpha_get_timeline (alpha);
  if (G_UNLIKELY (!timeline))
    {
      g_warning ("The passed ClutterAlpha does not have an "
                 "associated ClutterTimeline.");
      return NULL;
    }

  animation = g_object_get_qdata (G_OBJECT (actor), quark_actor_animation);
  if (G_LIKELY (!animation))
    {
      animation = clutter_animation_new ();
      CLUTTER_NOTE (ANIMATION, "Created new Animation [%p]", animation);
    }
  else
    CLUTTER_NOTE (ANIMATION, "Reusing Animation [%p]", animation);

  clutter_animation_set_timeline (animation, timeline);
  clutter_animation_set_alpha (animation, alpha);
  clutter_animation_set_actor (animation, actor);

  va_start (args, first_property_name);
  clutter_animation_setup_valist (animation, first_property_name, args);
  va_end (args);

  return animation;
}

/**
 * clutter_actor_animate_with_timeline:
 * @actor: a #ClutterActor
 * @mode: a #ClutterAnimationMode value
 * @timeline: a #ClutterTimeline
 * @first_property_name: the name of a property
 * @VarArgs: a %NULL terminated list of property names and
 *   property values
 *
 * Animates the given list of properties of @actor between the current
 * value for each property and a new final value. The animation has a
 * definite duration given by @timeline and a speed given by the @mode.
 *
 * See clutter_actor_animate() for further details.
 *
 * This function is useful if you want to use an existing timeline
 * to animate @actor.
 *
 * Return value: a #ClutterAnimation object. The object is owned by the
 *   #ClutterActor and should not be unreferenced with g_object_unref()
 *
 * Since: 1.0
 */
ClutterAnimation *
clutter_actor_animate_with_timeline (ClutterActor         *actor,
                                     ClutterAnimationMode  mode,
                                     ClutterTimeline      *timeline,
                                     const gchar          *first_property_name,
                                     ...)
{
  ClutterAnimation *animation;
  va_list args;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);
  g_return_val_if_fail (CLUTTER_IS_TIMELINE (timeline), NULL);
  g_return_val_if_fail (first_property_name != NULL, NULL);

  animation = g_object_get_qdata (G_OBJECT (actor), quark_actor_animation);
  if (G_LIKELY (!animation))
    {
      animation = clutter_animation_new ();
      CLUTTER_NOTE (ANIMATION, "Created new Animation [%p]", animation);
    }
  else
    CLUTTER_NOTE (ANIMATION, "Reusing Animation [%p]", animation);

  clutter_animation_set_timeline (animation, timeline);
  clutter_animation_set_alpha (animation, NULL);
  clutter_animation_set_mode (animation, mode);
  clutter_animation_set_actor (animation, actor);

  va_start (args, first_property_name);
  clutter_animation_setup_valist (animation, first_property_name, args);
  va_end (args);

  return animation;
}

/**
 * clutter_actor_animate:
 * @actor: a #ClutterActor
 * @mode: a #ClutterAnimationMode value
 * @duration: duration of the animation, in milliseconds
 * @first_property_name: the name of a property
 * @VarArgs: a %NULL terminated list of property names and
 *   property values
 *
 * Animates the given list of properties of @actor between the current
 * value for each property and a new final value. The animation has a
 * definite duration and a speed given by the @mode.
 *
 * For example, this:
 *
 * |[
 *   clutter_actor_animate (rectangle, CLUTTER_LINEAR, 250,
 *                          "width", 100,
 *                          "height", 100,
 *                          NULL);
 * ]|
 *
 * will make width and height properties of the #ClutterActor "rectangle"
 * grow linearly between the current value and 100 pixels, in 250 milliseconds.
 *
 * All the properties specified will be animated between the current value
 * and the final value. If a property should be set at the beginning of
 * the animation but not updated during the animation, it should be prefixed
 * by the "fixed::" string, for instance:
 *
 * |[
 *   clutter_actor_animate (actor, CLUTTER_EASE_IN, 100,
 *                          "rotation-angle-z", 360,
 *                          "fixed::rotation-center-x", 100,
 *                          "fixed::rotation-center-y", 100,
 *                          NULL);
 * ]|
 *
 * Will animate the "rotation-angle-z" property between the current value
 * and 360 degrees, and set the "rotation-center-x" and "rotation-center-y"
 * to the fixed value of 100 pixels.
 *
 * This function will implicitly create a #ClutterAnimation object which
 * will be assigned to the @actor and will be returned to the developer
 * to control the animation or to know when the animation has been
 * completed.
 *
 * Calling this function on an actor that is already being animated
 * will cause the current animation to change with the new final value.
 *
 * <note>Unless the animation is looping, it will become invalid as soon
 * as it is complete. To avoid this, you should keep a reference on the
 * returned value using g_object_ref().</note>
 *
 * Return value: a #ClutterAnimation object. The object is owned by the
 *   #ClutterActor and should not be unreferenced with g_object_unref()
 *
 * Since: 1.0
 */
ClutterAnimation *
clutter_actor_animate (ClutterActor         *actor,
                       ClutterAnimationMode  mode,
                       guint                 duration,
                       const gchar          *first_property_name,
                       ...)
{
  ClutterAnimation *animation;
  va_list args;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);
  g_return_val_if_fail (mode != CLUTTER_CUSTOM_MODE, NULL);
  g_return_val_if_fail (duration > 0, NULL);
  g_return_val_if_fail (first_property_name != NULL, NULL);

  animation = g_object_get_qdata (G_OBJECT (actor), quark_actor_animation);
  if (G_LIKELY (!animation))
    {
      /* if there is no animation already attached to the actor,
       * create one and set up the timeline and alpha using the
       * current values for duration, mode and loop
       */
      animation = clutter_animation_new ();
      clutter_animation_set_timeline (animation, NULL);
      clutter_animation_set_alpha (animation, NULL);
      clutter_animation_set_actor (animation, actor);

      CLUTTER_NOTE (ANIMATION, "Created new Animation [%p]", animation);
    }
  else
    CLUTTER_NOTE (ANIMATION, "Reusing Animation [%p]", animation);

  /* force the update of duration and mode using the new
   * values coming from the parameters of this function
   */
  clutter_animation_set_duration (animation, duration);
  clutter_animation_set_mode (animation, mode);

  va_start (args, first_property_name);
  clutter_animation_setup_valist (animation, first_property_name, args);
  va_end (args);

  return animation;
}