#include "CameraKeyframeHandler.h"

namespace playback::keyframe {

void CameraRenderStateHandler::applyCamera(CameraKeyframeChange const& change) {
    mState    = change.toRenderState();
    mDistance = change.distance();
}

std::optional<CameraRenderState> const& CameraRenderStateHandler::state() const noexcept { return mState; }

float CameraRenderStateHandler::distance() const noexcept { return mDistance; }

} // namespace playback::keyframe
