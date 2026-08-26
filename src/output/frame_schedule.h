#pragma once

extern "C" {
#include <wlr/backend/session.h>
}

namespace umbriel {

  enum class OutputFrameFollowup {
    None,
    Schedule,
    RetryDelayed,
  };

  [[nodiscard]] inline bool outputFrameAllowed(bool stopping, const wlr_session* session) {
    return !stopping && (session == nullptr || session->active);
  }

  [[nodiscard]] inline OutputFrameFollowup
  outputFrameFollowup(bool stopping, const wlr_session* session, bool commitFailed, bool animationsActive) {
    if (!outputFrameAllowed(stopping, session)) {
      return OutputFrameFollowup::None;
    }
    if (commitFailed) {
      return OutputFrameFollowup::RetryDelayed;
    }
    return animationsActive ? OutputFrameFollowup::Schedule : OutputFrameFollowup::None;
  }

} // namespace umbriel
