#include "CameraCommands.h"

#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/state/editing/CameraBindingOps.h"

#include <algorithm>
#include <map>
#include <set>
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
    if (CameraBindingOps::addFreeCamera(state, mName).empty()) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        AddFreeCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddFreeCamera::label() const { return "Add Free Camera"; }

DeleteCamera::DeleteCamera(std::string id) : mId(std::move(id)) {}

void DeleteCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!CameraBindingOps::deleteCamera(state, mId)) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        DeleteCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteCamera::label() const { return "Delete Camera"; }

CreateBindingCamera::CreateBindingCamera(std::string subActorId, std::string name)
: mSubActorId(std::move(subActorId)),
  mName(std::move(name)) {}

void CreateBindingCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    if (CameraBindingOps::createBindingCamera(state, mSubActorId, mName).empty()) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        CreateBindingCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string CreateBindingCamera::label() const { return "Create Binding Camera"; }

UnbindCamera::UnbindCamera(std::string id) : mId(std::move(id)) {}

void UnbindCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!CameraBindingOps::unbindCamera(state, mId)) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        UnbindCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string UnbindCamera::label() const { return "Unbind Camera"; }

AddKeyframe::AddKeyframe(std::string cameraId, int tick) : mCameraId(std::move(cameraId)), mTick(tick) {}

AddKeyframe::AddKeyframe(std::string cameraId, int tick, std::optional<model::CameraKeyframe> captured)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mCaptured(std::move(captured)) {}

void AddKeyframe::execute(model::EditorStateExt& state) {
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
}

void        AddKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddKeyframe::label() const { return "Add Keyframe"; }

MoveKeyframe::MoveKeyframe(std::string cameraId, int fromTick, int toTick)
: mCameraId(std::move(cameraId)),
  mFromTick(fromTick),
  mToTick(toTick) {}

void MoveKeyframe::execute(model::EditorStateExt& state) {
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
}

void        MoveKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveKeyframe::label() const { return "Move Keyframe"; }

DeleteKeyframe::DeleteKeyframe(std::string cameraId, int tick) : mCameraId(std::move(cameraId)), mTick(tick) {}

void DeleteKeyframe::execute(model::EditorStateExt& state) {
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
}

void        SetKeyframeInterpolation::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframeInterpolation::label() const { return "Set Keyframe Interpolation"; }

SetCameraTrackState::SetCameraTrackState(std::string cameraId, Property property, bool value)
: mCameraId(std::move(cameraId)),
  mProperty(property),
  mValue(value) {}

void SetCameraTrackState::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera) {
        mBefore.reset();
        return;
    }

    bool* target = mProperty == Property::Locked ? &camera->locked : &camera->enabled;
    if (*target == mValue) {
        mBefore.reset();
        return;
    }

    mBefore = state;
    *target = mValue;
}

void SetCameraTrackState::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraTrackState::label() const {
    return mProperty == Property::Locked ? "Set Camera Track Locked" : "Set Camera Track Enabled";
}

RenameCamera::RenameCamera(std::string cameraId, std::string name)
: mCameraId(std::move(cameraId)),
  mName(std::move(name)) {}

void RenameCamera::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked || mName.empty() || camera->name == mName) {
        mBefore.reset();
        return;
    }

    mBefore      = state;
    camera->name = mName;
}

void        RenameCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RenameCamera::label() const { return "Rename Camera"; }

ClearCameraKeyframes::ClearCameraKeyframes(std::string cameraId) : mCameraId(std::move(cameraId)) {}

void ClearCameraKeyframes::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked || camera->keysByTick.empty()) {
        mBefore.reset();
        return;
    }

    mBefore = state;
    camera->keysByTick.clear();
}

void        ClearCameraKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string ClearCameraKeyframes::label() const { return "Clear Camera Keyframes"; }

SetCameraKeyframePosition::SetCameraKeyframePosition(std::string cameraId, int tick, model::Vec3 position)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mPosition(position) {}

void SetCameraKeyframePosition::execute(model::EditorStateExt& state) {
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
}

void SetCameraKeyframePosition::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraKeyframePosition::label() const { return "Set Keyframe Position"; }

SetCameraKeyframeRotation::SetCameraKeyframeRotation(std::string cameraId, int tick, model::Vec3 rotation)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mRotation(rotation) {}

void SetCameraKeyframeRotation::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end()
        || key->second.yaw == mRotation.x && key->second.pitch == mRotation.y && key->second.roll == mRotation.z) {
        mBefore.reset();
        return;
    }

    mBefore           = state;
    key->second.yaw   = mRotation.x;
    key->second.pitch = mRotation.y;
    key->second.roll  = mRotation.z;
}

void SetCameraKeyframeRotation::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraKeyframeRotation::label() const { return "Set Keyframe Rotation"; }

SetCameraKeyframeFov::SetCameraKeyframeFov(std::string cameraId, int tick, float fov)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mFov(fov) {}

void SetCameraKeyframeFov::execute(model::EditorStateExt& state) {
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
}

void        SetCameraKeyframeFov::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeFov::label() const { return "Set Keyframe FOV"; }

MoveKeyframes::MoveKeyframes(std::vector<model::KeyframeRef> refs, int delta) : mRefs(std::move(refs)), mDelta(delta) {}

void MoveKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (mRefs.empty() || mDelta == 0) return;

    std::map<std::string, std::set<int>> moving;
    for (auto const& ref : mRefs) moving[ref.cameraId].insert(ref.tick);

    for (auto const& [cameraId, ticks] : moving) {
        auto const* camera = findCamera(state, cameraId);
        if (!camera || camera->locked) return;
        for (int const tick : ticks) {
            int const target = tick + mDelta;
            if (!camera->keysByTick.contains(tick) || target < 0 || target > state.totalTicks) return;
            if (camera->keysByTick.contains(target) && !ticks.contains(target)) return;
        }
    }

    auto before = state;
    for (auto const& [cameraId, ticks] : moving) {
        auto*                                camera = findCamera(state, cameraId);
        std::vector<std::pair<int, model::CameraKeyframe>> lifted;
        for (int const tick : ticks) {
            auto node = camera->keysByTick.extract(tick);
            lifted.emplace_back(tick + mDelta, std::move(node.mapped()));
        }
        for (auto& [tick, key] : lifted) camera->keysByTick.emplace(tick, std::move(key));
    }
    mBefore = std::move(before);
}

void        MoveKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveKeyframes::label() const { return "Move Keyframes"; }

DeleteKeyframes::DeleteKeyframes(std::vector<model::KeyframeRef> refs) : mRefs(std::move(refs)) {}

void DeleteKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    for (auto const& ref : mRefs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked) continue;
        changed |= camera->keysByTick.erase(ref.tick) > 0;
    }
    if (changed) mBefore = std::move(before);
}

void        DeleteKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteKeyframes::label() const { return "Delete Keyframes"; }

SetKeyframesInterpolation::SetKeyframesInterpolation(
    std::vector<model::KeyframeRef> refs,
    model::CameraInterpolationType  interpolation
)
: mRefs(std::move(refs)),
  mInterpolation(interpolation) {}

void SetKeyframesInterpolation::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    for (auto const& ref : mRefs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked) continue;
        auto const key = camera->keysByTick.find(ref.tick);
        if (key == camera->keysByTick.end() || key->second.interpolationType == mInterpolation) continue;
        key->second.interpolationType = mInterpolation;
        changed                       = true;
    }
    if (changed) mBefore = std::move(before);
}

void        SetKeyframesInterpolation::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframesInterpolation::label() const { return "Set Keyframes Interpolation"; }

AddKeyframes::AddKeyframes(std::vector<model::KeyframeRef> refs) : mRefs(std::move(refs)) {}

void AddKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    for (auto const& ref : mRefs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked || !ref.pose || ref.tick < 0 || ref.tick > state.totalTicks) continue;
        changed |= camera->keysByTick.emplace(ref.tick, *ref.pose).second;
    }
    if (changed) mBefore = std::move(before);
}

void        AddKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddKeyframes::label() const { return "Paste Keyframes"; }

} // namespace playback::state::editing::command
