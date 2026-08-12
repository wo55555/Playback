#pragma once

#include "CameraRenderState.h"
#include "playback/editor/editing/models/CameraKeyframe.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace playback::editor::keyframe {

struct KeyframeTrackRange {
    int startTick{};
    int endTick{};
};

class KeyframeTrack {
public:
    explicit KeyframeTrack(std::span<editing::model::CameraKeyframe const> keyframes);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::optional<CameraRenderState> sample(long double tick) const;

    // Returns the active segment plus one adjacent segment on each side for
    // the camera-path visualization window.
    [[nodiscard]] std::optional<KeyframeTrackRange> surroundingRange(long double tick) const noexcept;
    [[nodiscard]] std::vector<CameraRenderState>
    sampleRange(long double startTick, long double endTick, size_t maxSamples) const;

private:
    [[nodiscard]] CameraRenderState stateFromKeyframe(editing::model::CameraKeyframe const& key) const;

    std::vector<editing::model::CameraKeyframe> mKeyframes;
};

} // namespace playback::editor::keyframe
