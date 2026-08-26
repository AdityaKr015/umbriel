#include "check.h"
#include "output/frame_schedule.h"

using umbriel::OutputFrameFollowup;
using umbriel::outputFrameFollowup;

UMBRIEL_TEST(nestedBackendCanScheduleFrames) {
  CHECK_EQ(outputFrameFollowup(false, nullptr, false, true), OutputFrameFollowup::Schedule);
}

UMBRIEL_TEST(activeNativeSessionCanScheduleFrames) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, false, true), OutputFrameFollowup::Schedule);
}

UMBRIEL_TEST(inactiveNativeSessionCannotScheduleFrames) {
  wlr_session session{};
  session.active = false;
  CHECK_EQ(outputFrameFollowup(false, &session, false, true), OutputFrameFollowup::None);
}

UMBRIEL_TEST(stoppingCompositorCannotScheduleFrames) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(true, &session, false, true), OutputFrameFollowup::None);
}

UMBRIEL_TEST(failedCommitUsesDelayedRetry) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, true, false), OutputFrameFollowup::RetryDelayed);
}

UMBRIEL_TEST(failedAnimatedFrameDoesNotScheduleImmediately) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, true, true), OutputFrameFollowup::RetryDelayed);
}

UMBRIEL_TEST(idleSuccessfulFrameNeedsNoFollowup) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, false, false), OutputFrameFollowup::None);
}

int main() { return RUN_TESTS(); }
