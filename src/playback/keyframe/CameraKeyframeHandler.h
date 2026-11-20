#pragma once

#include "CameraKeyframeChange.h"

#include <optional>

namespace playback::keyframe {

class CameraKeyframeHandler {
public:
    virtual ~CameraKeyframeHandler()                             = default;
    virtual void applyCamera(CameraKeyframeChange const& change) = 0;
};

class CameraRenderStateHandler final : public CameraKeyframeHandler {
public:
    void applyCamera(CameraKeyframeChange const& change) override;

    [[nodiscard]] std::optional<CameraRenderState> const& state() const noexcept;
    [[nodiscard]] float                                   distance() const noexcept;

private:
    std::optional<CameraRenderState> mState;
    float                            mDistance{0.0f};
};

} // namespace playback::keyframe
