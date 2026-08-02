#pragma once

#include "CameraKeyframe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace playback::editor::editing::model {

enum class CameraKind : uint8_t { Keyframe = 0, Path, Rig, Preset };

struct CameraEntity {
    std::string                 id;
    std::string                 name;
    CameraKind                  kind{CameraKind::Keyframe};
    std::vector<CameraKeyframe> keys;
    std::string                 bindingEntityUuid;
    bool                        active{};
    bool                        locked{};
};

} // namespace playback::editor::editing::model
