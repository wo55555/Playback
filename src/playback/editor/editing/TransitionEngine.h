#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/Track.h"

#include <optional>
#include <string>

namespace playback::editor::editing {

struct RenderPlan {
    std::string primaryClipId;
    std::optional<std::string> secondaryClipId;  // set during transition
    float blendAlpha{1.0f};                      // 0=primary, 1=secondary
    model::TransitionKind kind{model::TransitionKind::Cut};
};

class TransitionEngine {
public:
    // Called per frame during render
    // Input: current timeline tick + full editor state
    // Output: which clips to render and how to blend
    static RenderPlan planAt(int timelineTick, const model::EditorStateExt& editor);

private:
    static float easingValue(int easing, float t);
};

} // namespace playback::editor::editing
