#pragma once

#include "CameraKeyframeChange.h"
#include "playback/state/editing/models/CameraKeyframe.h"

#include <cstddef>
#include <map>
#include <optional>

namespace playback::keyframe {

class KeyframeTrack {
public:
    using KeyMap     = std::map<int, state::editing::model::CameraKeyframe>;
    using KeyMapIter = KeyMap::const_iterator;

    explicit KeyframeTrack(KeyMap const& keyframes);

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::optional<CameraKeyframeChange> createChange(long double tick) const;

private:
    [[nodiscard]] CameraKeyframeChange changeFromKeyframe(state::editing::model::CameraKeyframe const& key) const;

    [[nodiscard]] CameraKeyframeChange smoothChange(KeyMapIter left, float amount) const;

    [[nodiscard]] CameraKeyframeChange hermiteChange(KeyMapIter left, long double tick) const;

    KeyMap mKeyframes;
};

} // namespace playback::keyframe
