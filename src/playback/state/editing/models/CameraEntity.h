#pragma once

#include "CameraKeyframe.h"

#include <map>
#include <optional>
#include <string>

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

// Keyframe tracks interpolate their own poses; person tracks view a replay player between their first and last key.
enum class CameraTrackKind : uint8_t { Keyframe, FirstPerson, ThirdPerson };

struct CameraEntity {
    std::string                   id;
    std::string                   name;
    CameraTrackKind               kind{CameraTrackKind::Keyframe};
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

} // namespace playback::state::editing::model
