#include <stdlib.h>
#include <glib.h>
#include <clutter/clutter.h>

#include "test-conform-common.h"

/* We use a nice slow timeline for this test since we
 * dont want the timeouts to interpolate the timeline
 * forward multiple frames */
#define TEST_TIMELINE_FPS 10
#define TEST_TIMELINE_FRAME_COUNT 20

typedef struct _TestState {
  ClutterTimeline *timeline;
  gint prev_frame;
  gint completion_count;
  gint passed;
}TestState;


static void
new_frame_cb (ClutterTimeline *timeline,
              gint frame_num,
              TestState *state)
{
  gint current_frame = clutter_timeline_get_current_frame (state->timeline);
  
  if (state->prev_frame
      != clutter_timeline_get_current_frame (state->timeline))
    {
      g_test_message ("timeline previous frame=%-4i "
		      "actual frame=%-4i (OK)\n",
		      state->prev_frame,
		      current_frame);
    }
  else
    {
      g_test_message ("timeline previous frame=%-4i "
		      "actual frame=%-4i (FAILED)\n",
		      state->prev_frame,
		      current_frame);

      state->passed = FALSE;
    }

  state->prev_frame = current_frame;
}


static void
completed_cb (ClutterTimeline *timeline,
	      TestState *state)
{
  state->completion_count++;

  if (state->completion_count == 2)
    {
      if (state->passed)
	{
	  g_test_message ("Passed\n");
	  clutter_main_quit ();
	}
      else
	{
	  g_test_message ("Failed\n");
	  exit (EXIT_FAILURE);
	}
    }
}


void
test_timeline_dup_frames (TestConformSimpleFixture *fixture,
			  gconstpointer data)
{
  TestState state;

  state.timeline = 
    clutter_timeline_new (TEST_TIMELINE_FRAME_COUNT,
			  TEST_TIMELINE_FPS);
  clutter_timeline_set_loop (state.timeline, TRUE);
  g_signal_connect (G_OBJECT(state.timeline),
		    "new-frame",
		    G_CALLBACK(new_frame_cb),
		    &state);
  g_signal_connect (G_OBJECT(state.timeline),
		    "completed",
		    G_CALLBACK(completed_cb),
		    &state);

  state.prev_frame = -1;
  state.completion_count = 0;
  state.passed = TRUE;

  clutter_timeline_start (state.timeline);

  clutter_main();

  g_object_unref (state.timeline);
}
