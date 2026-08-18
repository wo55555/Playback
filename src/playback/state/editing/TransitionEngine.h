#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/Track.h"

#include <optional>
#include <string>

namespace playback::state::editing {

struct RenderPlan {
    std::string                primaryClipId;
    std::optional<std::string> secondaryClipId;  // set during transition
    float                      blendAlpha{1.0f}; // 0=primary, 1=secondary
    model::TransitionKind      kind{model::TransitionKind::Cut};
};

class TransitionEngine {
public:
    static RenderPlan planAt(int timelineTick, const model::EditorStateExt& editor);

private:
    static float easingValue(int easing, float t);
};

} // namespace playback::state::editing
