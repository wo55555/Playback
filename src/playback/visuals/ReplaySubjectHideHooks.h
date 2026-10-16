#pragma once

#include "ReplayEntityInterpolator.h"

namespace playback::visuals {

// Skips drawing the hidden replay subject (body, hands, held item) while its render scope is active; the invisible
// flags alone only remove the head. Stays installed while any owner (export, follow preview) still needs it.
void hookReplaySubjectHide(bool enable, SubjectHideReason owner);

} // namespace playback::visuals
