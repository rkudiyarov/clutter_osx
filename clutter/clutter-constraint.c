/**
 * SECTION:clutter-constraint
 * @Title: ClutterConstraint
 * @Short_Description: Abstract class for constraints on position or size
 * @See_Also: #ClutterAction
 *
 * #ClutterConstraint is a base abstract class for modifiers of a #ClutterActor
 * position or size.
 *
 * A #ClutterConstraint sub-class should contain the logic for modifying
 * the position or size of the #ClutterActor to which it is applied, using
 * the various signals and properties of #ClutterActor itself.
 *
 * #ClutterConstraint is available since Clutter 1.4
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-constraint.h"

#include "clutter-actor-meta-private.h"

G_DEFINE_ABSTRACT_TYPE (ClutterConstraint,
                        clutter_constraint,
                        CLUTTER_TYPE_ACTOR_META);

static void
clutter_constraint_class_init (ClutterConstraintClass *klass)
{
}

static void
clutter_constraint_init (ClutterConstraint *self)
{
}
