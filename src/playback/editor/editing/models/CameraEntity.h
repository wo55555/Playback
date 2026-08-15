#pragma once

#include "CameraKeyframe.h"

#include <map>
#include <optional>
#include <string>

namespace playback::editor::editing::model {

struct CameraShake {
    int   startTick{};
    int   endTick{};
    float positionAmplitude{};
    float rotationAmplitude{};
    float frequency{1.0f};
};
struct CameraLimiter {
    Vec3 min{};
    Vec3 max{};
    bool enabled{};
};

struct CameraEntity {
    std::string id;
    std::string name;
    bool                          enabled{true};
    std::map<int, CameraKeyframe> keysByTick;
    std::optional<CameraShake>    shake;
    std::optional<CameraLimiter>  limiter;
    std::string                   bindingEntityUuid;
    int                           bindingMode{};
    float                         bindingDamping{0.1f};
    bool                          locked{};
};

[[nodiscard]] inline bool hasCameraSource(CameraEntity const& camera) noexcept { return !camera.keysByTick.empty(); }

[[nodiscard]] inline bool isCameraRenderable(CameraEntity const& camera) noexcept {
    return camera.enabled && hasCameraSource(camera);
}

} // namespace playback::editor::editing::model
