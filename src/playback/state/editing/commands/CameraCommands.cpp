#include "CameraCommands.h"

#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/state/editing/CameraBindingOps.h"

#include <algorithm>
#include <utility>

namespace playback::state::editing::command {
namespace {

model::CameraEntity* findCamera(model::EditorStateExt& state, std::string const& id) {
    auto it =
        std::find_if(state.cameras.begin(), state.cameras.end(), [&id](auto const& camera) { return camera.id == id; });
    return it == state.cameras.end() ? nullptr : &*it;
}

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

} // namespace

AddFreeCamera::AddFreeCamera(std::string name) : mName(std::move(name)) {}

void AddFreeCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = !CameraBindingOps::addFreeCamera(state, mName).empty();
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        AddFreeCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddFreeCamera::label() const { return "Add Free Camera"; }

DeleteCamera::DeleteCamera(std::string id) : mId(std::move(id)) {}

void DeleteCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = CameraBindingOps::deleteCamera(state, mId);
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        DeleteCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteCamera::label() const { return "Delete Camera"; }

CreateBindingCamera::CreateBindingCamera(std::string subActorId, std::string name)
: mSubActorId(std::move(subActorId)),
  mName(std::move(name)) {}

void CreateBindingCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = !CameraBindingOps::createBindingCamera(state, mSubActorId, mName).empty();
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        CreateBindingCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string CreateBindingCamera::label() const { return "Create Binding Camera"; }

UnbindCamera::UnbindCamera(std::string id) : mId(std::move(id)) {}

void UnbindCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = CameraBindingOps::unbindCamera(state, mId);
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        UnbindCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string UnbindCamera::label() const { return "Unbind Camera"; }

AddKeyframe::AddKeyframe(std::string cameraId, int tick) : mCameraId(std::move(cameraId)), mTick(tick) {}

AddKeyframe::AddKeyframe(std::string cameraId, int tick, std::optional<model::CameraKeyframe> captured)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mCaptured(std::move(captured)) {}

void AddKeyframe::execute(model::EditorStateExt& state) {
    mChanged          = false;
    auto*      camera = findCamera(state, mCameraId);
    auto const tick   = std::clamp(mTick, 0, state.totalTicks);
    if (!camera || camera->locked || camera->keysByTick.contains(tick)) {
        mBefore.reset();
        return;
    }

    mBefore = state;
    model::CameraKeyframe key;
    if (mCaptured) key = *mCaptured;
    else if (!camera->keysByTick.empty()) key = camera->keysByTick.rbegin()->second;
    else if (auto captured = keyframe::captureClientCamera()) key.fov = captured->fov;
    camera->keysByTick.emplace(tick, std::move(key));
    mChanged = true;
}

void        AddKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddKeyframe::label() const { return "Add Keyframe"; }

MoveKeyframe::MoveKeyframe(std::string cameraId, int fromTick, int toTick)
: mCameraId(std::move(cameraId)),
  mFromTick(fromTick),
  mToTick(toTick) {}

void MoveKeyframe::execute(model::EditorStateExt& state) {
    mChanged            = false;
    auto*      camera   = findCamera(state, mCameraId);
    auto const fromTick = std::clamp(mFromTick, 0, state.totalTicks);
    auto const toTick   = std::clamp(mToTick, 0, state.totalTicks);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const source = camera->keysByTick.find(fromTick);
    if (source == camera->keysByTick.end() || fromTick == toTick || camera->keysByTick.contains(toTick)) {
        mBefore.reset();
        return;
    }

    mBefore  = state;
    auto key = std::move(source->second);
    camera->keysByTick.erase(source);
    camera->keysByTick.emplace(toTick, std::move(key));
    mChanged = true;
}

void        MoveKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveKeyframe::label() const { return "Move Keyframe"; }

DeleteKeyframe::DeleteKeyframe(std::string cameraId, int tick) : mCameraId(std::move(cameraId)), mTick(tick) {}

void DeleteKeyframe::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end()) {
        mBefore.reset();
        return;
    }

    mBefore = state;
    camera->keysByTick.erase(key);
    mChanged = true;
}

void        DeleteKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteKeyframe::label() const { return "Delete Keyframe"; }

SetKeyframeInterpolation::SetKeyframeInterpolation(
    std::string                    cameraId,
    int                            tick,
    model::CameraInterpolationType interpolation
)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mInterpolation(interpolation) {}

void SetKeyframeInterpolation::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end() || key->second.interpolationType == mInterpolation) {
        mBefore.reset();
        return;
    }

    mBefore                       = state;
    key->second.interpolationType = mInterpolation;
    mChanged                      = true;
}

void        SetKeyframeInterpolation::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframeInterpolation::label() const { return "Set Keyframe Interpolation"; }

SetCameraTrackState::SetCameraTrackState(std::string cameraId, Property property, bool value)
: mCameraId(std::move(cameraId)),
  mProperty(property),
  mValue(value) {}

void SetCameraTrackState::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera) {
        mBefore.reset();
        return;
    }

    bool* target = &camera->enabled;
    if (*target == mValue) {
        mBefore.reset();
        return;
    }

    mBefore  = state;
    *target  = mValue;
    mChanged = true;
}

void SetCameraTrackState::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraTrackState::label() const { return "Set Camera Track Enabled"; }

SetCameraKeyframePosition::SetCameraKeyframePosition(std::string cameraId, int tick, model::Vec3 position)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mPosition(position) {}

void SetCameraKeyframePosition::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end()
        || key->second.position.x == mPosition.x && key->second.position.y == mPosition.y
               && key->second.position.z == mPosition.z) {
        mBefore.reset();
        return;
    }

    mBefore              = state;
    key->second.position = mPosition;
    mChanged             = true;
}

void SetCameraKeyframePosition::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraKeyframePosition::label() const { return "Set Keyframe Position"; }

SetCameraKeyframeFov::SetCameraKeyframeFov(std::string cameraId, int tick, float fov)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mFov(fov) {}

void SetCameraKeyframeFov::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end() || key->second.fov == mFov) {
        mBefore.reset();
        return;
    }

    mBefore         = state;
    key->second.fov = mFov;
    mChanged        = true;
}

void        SetCameraKeyframeFov::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeFov::label() const { return "Set Keyframe FOV"; }

} // namespace playback::state::editing::command
