#pragma once

#include "CameraKeyframe.h"
#include "Track.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::model {

struct CameraShake {
    int   startTick{};
    int   endTick{};
    float positionAmplitude{};
    float rotationAmplitude{};
    float frequency{1.0f};

    bool operator==(CameraShake const&) const = default;
};
struct CameraLimiter {
    Vec3 min{};
    Vec3 max{};
    bool enabled{};
};

struct CameraEntity {
    std::string                   id;
    std::string                   name;
    bool                          enabled{true};
    std::map<int, CameraKeyframe> keysByTick;
    std::vector<Clip>             clips;
    std::optional<CameraShake>    shake;
    std::optional<CameraLimiter>  limiter;
    bool                          locked{};
};

[[nodiscard]] inline bool hasCameraSource(CameraEntity const& camera) noexcept { return !camera.keysByTick.empty(); }

[[nodiscard]] inline bool isCameraRenderable(CameraEntity const& camera) noexcept {
    return camera.enabled && hasCameraSource(camera);
}

} // namespace playback::state::editing::model
