#pragma once

#include "ReplayEntityInterpolator.h"

namespace playback::visuals {

// Prevents held attachables (notably bows, which bind by matching parent bone name) from latching permanently to the
// local origin or keeping a stale parent skeletal hierarchy after skin or geometry swaps during replay.
void hookReplayAttachables(bool enable, SubjectHideReason owner);

} // namespace playback::visuals
