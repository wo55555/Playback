#pragma once

#include "CameraKeyframeChange.h"
#include "playback/editor/editing/models/CameraKeyframe.h"

#include <cstddef>
#include <map>
#include <optional>

namespace playback::editor::keyframe {

class KeyframeTrack {
public:
    using KeyMap     = std::map<int, editing::model::CameraKeyframe>;
    using KeyMapIter = KeyMap::const_iterator;

    explicit KeyframeTrack(KeyMap const& keyframes);

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::optional<CameraKeyframeChange> createChange(long double tick) const;

private:
    [[nodiscard]] CameraKeyframeChange changeFromKeyframe(editing::model::CameraKeyframe const& key) const;

    [[nodiscard]] CameraKeyframeChange smoothChange(KeyMapIter left, float amount) const;

    [[nodiscard]] CameraKeyframeChange hermiteChange(KeyMapIter left, long double tick) const;

    KeyMap mKeyframes;
};

} // namespace playback::editor::keyframe
