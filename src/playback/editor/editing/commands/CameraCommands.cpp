#include "CameraCommands.h"

#include "playback/editor/editing/CameraBindingOps.h"

#include <algorithm>
#include <utility>

namespace playback::editor::editing::command {
namespace {

model::CameraEntity* findCamera(model::EditorStateExt& state, std::string const& id) {
    auto it =
        std::find_if(state.cameras.begin(), state.cameras.end(), [&id](auto const& camera) { return camera.id == id; });
    return it == state.cameras.end() ? nullptr : &*it;
}

std::string makeKeyframeId(model::CameraEntity const& camera) {
    size_t next = camera.keys.size() + 1;
    for (;;) {
        auto id = camera.id + ".key." + std::to_string(next++);
        if (std::none_of(camera.keys.begin(), camera.keys.end(), [&id](auto const& key) { return key.id == id; })) {
            return id;
        }
    }
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
    if (!camera || camera->locked || std::any_of(camera->keys.begin(), camera->keys.end(), [tick](auto const& key) {
            return key.tick == tick;
        })) {
        mBefore.reset();
        return;
    }

    mBefore = state;
    model::CameraKeyframe key;
    if (mCaptured) key = *mCaptured;
    else if (!camera->keys.empty()) key = camera->keys.back();
    key.id   = makeKeyframeId(*camera);
    key.tick = tick;
    camera->keys.push_back(std::move(key));
    std::sort(camera->keys.begin(), camera->keys.end(), [](auto const& left, auto const& right) {
        return left.tick < right.tick;
    });
    mChanged = true;
}

void        AddKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddKeyframe::label() const { return "Add Keyframe"; }

MoveKeyframe::MoveKeyframe(std::string cameraId, std::string keyframeId, int tick)
: mCameraId(std::move(cameraId)),
  mKeyframeId(std::move(keyframeId)),
  mTick(tick) {}

void MoveKeyframe::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto       key        = std::find_if(camera->keys.begin(), camera->keys.end(), [&](auto const& value) {
        return value.id == mKeyframeId;
    });
    auto const targetTick = std::clamp(mTick, 0, state.totalTicks);
    if (key == camera->keys.end() || key->tick == targetTick
        || std::any_of(camera->keys.begin(), camera->keys.end(), [&](auto const& value) {
               return value.id != mKeyframeId && value.tick == targetTick;
           })) {
        mBefore.reset();
        return;
    }

    mBefore   = state;
    key->tick = targetTick;
    std::sort(camera->keys.begin(), camera->keys.end(), [](auto const& left, auto const& right) {
        return left.tick < right.tick;
    });
    mChanged = true;
}

void        MoveKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveKeyframe::label() const { return "Move Keyframe"; }

DeleteKeyframe::DeleteKeyframe(std::string cameraId, std::string keyframeId)
: mCameraId(std::move(cameraId)),
  mKeyframeId(std::move(keyframeId)) {}

void DeleteKeyframe::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto key = std::find_if(camera->keys.begin(), camera->keys.end(), [&](auto const& value) {
        return value.id == mKeyframeId;
    });
    if (key == camera->keys.end()) {
        mBefore.reset();
        return;
    }

    mBefore = state;
    camera->keys.erase(key);
    mChanged = true;
}

void        DeleteKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteKeyframe::label() const { return "Delete Keyframe"; }

SetKeyframeInterpolation::SetKeyframeInterpolation(
    std::string                    cameraId,
    std::string                    keyframeId,
    model::CameraInterpolationType interpolation
)
: mCameraId(std::move(cameraId)),
  mKeyframeId(std::move(keyframeId)),
  mInterpolation(interpolation) {}

void SetKeyframeInterpolation::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto key = std::find_if(camera->keys.begin(), camera->keys.end(), [&](auto const& value) {
        return value.id == mKeyframeId;
    });
    if (key == camera->keys.end() || key->interpolationType == mInterpolation) {
        mBefore.reset();
        return;
    }

    mBefore                = state;
    key->interpolationType = mInterpolation;
    mChanged               = true;
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

    bool* target = mProperty == Property::Enabled ? &camera->enabled : &camera->pathVisible;
    if (*target == mValue) {
        mBefore.reset();
        return;
    }

    mBefore  = state;
    *target  = mValue;
    mChanged = true;
}

void SetCameraTrackState::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraTrackState::label() const {
    return mProperty == Property::Enabled ? "Set Camera Track Enabled" : "Set Camera Path Visibility";
}

SetCameraKind::SetCameraKind(std::string cameraId, model::CameraKind kind)
: mCameraId(std::move(cameraId)),
  mKind(kind) {}

void SetCameraKind::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked || camera->kind == mKind || !model::hasCameraSource(*camera, mKind)) {
        mBefore.reset();
        return;
    }

    mBefore      = state;
    camera->kind = mKind;
    mChanged     = true;
}

void        SetCameraKind::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKind::label() const { return "Set Camera Kind"; }

} // namespace playback::editor::editing::command
