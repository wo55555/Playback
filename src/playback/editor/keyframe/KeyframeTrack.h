#pragma once

#include "CameraKeyframeChange.h"
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
    [[nodiscard]] std::optional<CameraKeyframeChange> createChange(long double tick) const;
    [[nodiscard]] std::optional<KeyframeTrackRange> surroundingRange(long double tick) const noexcept;

private:
    [[nodiscard]] CameraKeyframeChange changeFromKeyframe(editing::model::CameraKeyframe const& key) const;
    [[nodiscard]] CameraKeyframeChange smoothChange(size_t leftIndex, float amount) const;
    [[nodiscard]] CameraKeyframeChange hermiteChange(size_t leftIndex, long double tick) const;
    [[nodiscard]] editing::model::Vec3 samplePathPosition(size_t leftIndex, float amount) const;

    std::vector<editing::model::CameraKeyframe> mKeyframes;
};

} // namespace playback::editor::keyframe
