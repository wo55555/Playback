#pragma once

#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/editor/models/Track.h"

#include <optional>
#include <string>

namespace playback::refactor::video_editing {

using namespace playback::refactor::editor;

struct RenderPlan {
    std::string primaryClipId;
    std::optional<std::string> secondaryClipId;  // set during transition
    float blendAlpha{1.0f};                      // 0=primary, 1=secondary
    TransitionKind kind{TransitionKind::Cut};
};

class TransitionEngine {
public:
    // Called per frame during render
    // Input: current timeline tick + full editor state
    // Output: which clips to render and how to blend
    static RenderPlan planAt(int timelineTick, const EditorStateExt& editor);

private:
    static float easingValue(int easing, float t);
};

} // namespace playback::refactor::video_editing