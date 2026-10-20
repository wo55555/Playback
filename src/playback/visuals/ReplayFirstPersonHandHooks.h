#pragma once

#include "ReplayEntityInterpolator.h"

namespace playback::visuals {

// While the view is first-person on the recorded player (export, follow preview), the observer cannot show hands, so
// the native hand pass runs on the recorded player instead. Stays installed while any owner still needs it.
void hookReplayFirstPersonHands(bool enable, SubjectHideReason owner);

// True inside the swapped hand pass, where the recorded player's own model must not be hidden.
[[nodiscard]] bool isReplayFirstPersonHandPass();

// True while the native nausea distortion is reshaping the view matrix, which then differs from the camera sample.
[[nodiscard]] bool isReplayNauseaDistortionActive();

} // namespace playback::visuals
