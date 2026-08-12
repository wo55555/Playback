#include "CameraKeyframeHandler.h"

namespace playback::editor::keyframe {

void CameraRenderStateHandler::applyCamera(CameraKeyframeChange const& change) { mState = change.toRenderState(); }

std::optional<CameraRenderState> const& CameraRenderStateHandler::state() const noexcept { return mState; }

} // namespace playback::editor::keyframe
