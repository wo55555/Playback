#include "CameraCommands.h"

#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/state/editing/CameraBindingOps.h"
#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <cmath>
#include <iterator>
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
    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = std::move(before);
}

void        AddFreeCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddFreeCamera::label() const { return "playback.refactorEditor.history.addFreeCamera"; }

DeleteCamera::DeleteCamera(std::string id) : mId(std::move(id)) {}

void DeleteCamera::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!CameraBindingOps::deleteCamera(state, mId)) {
        mBefore.reset();
        return;
    }
    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = std::move(before);
}

void        DeleteCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteCamera::label() const { return "playback.refactorEditor.history.deleteCamera"; }

AddKeyframe::AddKeyframe(std::string cameraId, int tick) : mCameraId(std::move(cameraId)), mTick(tick) {}

AddKeyframe::AddKeyframe(std::string cameraId, int tick, std::optional<model::CameraKeyframe> captured)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mCaptured(std::move(captured)) {}

void AddKeyframe::execute(model::EditorStateExt& state) {
    auto*      camera = findCamera(state, mCameraId);
    auto const tick   = std::clamp(mTick, 0, state.totalTicks);
    if (!camera || camera->locked) {
        mBefore.reset();
        mReplaced = false;
        return;
    }

    mBefore   = state;
    mReplaced = camera->keysByTick.contains(tick);
    model::CameraKeyframe key;
    if (mCaptured) {
        key = *mCaptured;
    } else if (!camera->keysByTick.empty()) {
        key = camera->keysByTick.rbegin()->second;
        // New keyframes default to interpolated lens mode rather than inheriting a person keyframe.
        key.mode = model::CameraKeyframeMode::Interpolated;
    } else if (auto captured = keyframe::captureClientCamera()) {
        key.fov = captured->fov;
    }
    camera->keysByTick.insert_or_assign(tick, std::move(key));
}

void        AddKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddKeyframe::label() const {
    return mReplaced ? "playback.refactorEditor.history.replaceKeyframe"
                     : "playback.refactorEditor.history.addKeyframe";
}

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
std::string MoveKeyframe::label() const { return "playback.refactorEditor.history.moveKeyframe"; }

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
std::string DeleteKeyframe::label() const { return "playback.refactorEditor.history.deleteKeyframe"; }

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
std::string SetKeyframeInterpolation::label() const {
    return "playback.refactorEditor.history.setKeyframeInterpolation";
}

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
    return mProperty == Property::Locked ? "playback.refactorEditor.history.setCameraTrackLocked"
                                         : "playback.refactorEditor.history.setCameraTrackEnabled";
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
std::string RenameCamera::label() const { return "playback.refactorEditor.history.renameCamera"; }

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
std::string ClearCameraKeyframes::label() const { return "playback.refactorEditor.history.clearCameraKeyframes"; }

SetCameraKeyframePosition::SetCameraKeyframePosition(std::string cameraId, int tick, model::Vec3 position)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mPosition(position) {}

void SetCameraKeyframePosition::execute(model::EditorStateExt& state) {
    if (!std::isfinite(mPosition.x) || !std::isfinite(mPosition.y) || !std::isfinite(mPosition.z)) {
        mBefore.reset();
        return;
    }

    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end()
        || (key->second.position.x == mPosition.x && key->second.position.y == mPosition.y
            && key->second.position.z == mPosition.z)) {
        mBefore.reset();
        return;
    }

    mBefore              = state;
    key->second.position = mPosition;
}

void SetCameraKeyframePosition::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraKeyframePosition::label() const { return "playback.refactorEditor.history.setKeyframePosition"; }

SetCameraKeyframeRotation::SetCameraKeyframeRotation(std::string cameraId, int tick, model::Vec3 rotation)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mRotation(rotation) {}

void SetCameraKeyframeRotation::execute(model::EditorStateExt& state) {
    if (!std::isfinite(mRotation.x) || !std::isfinite(mRotation.y) || !std::isfinite(mRotation.z)) {
        mBefore.reset();
        return;
    }

    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end()
        || (key->second.yaw == mRotation.x && key->second.pitch == mRotation.y && key->second.roll == mRotation.z)) {
        mBefore.reset();
        return;
    }

    mBefore           = state;
    key->second.yaw   = mRotation.x;
    key->second.pitch = mRotation.y;
    key->second.roll  = mRotation.z;
}

void SetCameraKeyframeRotation::undo(model::EditorStateExt& state) { restore(mBefore, state); }

std::string SetCameraKeyframeRotation::label() const { return "playback.refactorEditor.history.setKeyframeRotation"; }

SetCameraKeyframeFov::SetCameraKeyframeFov(std::string cameraId, int tick, float fov)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mFov(fov) {}

void SetCameraKeyframeFov::execute(model::EditorStateExt& state) {
    if (!std::isfinite(mFov)) {
        mBefore.reset();
        return;
    }

    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const  key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    float const fov = std::clamp(mFov, 1.0f, 179.0f);
    if (key == camera->keysByTick.end() || key->second.fov == fov) {
        mBefore.reset();
        return;
    }

    mBefore         = state;
    key->second.fov = fov;
}

void        SetCameraKeyframeFov::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeFov::label() const { return "playback.refactorEditor.history.setKeyframeFov"; }

SetCameraKeyframeSubject::SetCameraKeyframeSubject(std::string cameraId, int tick, std::string subject)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mSubject(std::move(subject)) {}

void SetCameraKeyframeSubject::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end() || key->second.subject == mSubject) {
        mBefore.reset();
        return;
    }

    mBefore             = state;
    key->second.subject = mSubject;
}

void        SetCameraKeyframeSubject::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeSubject::label() const { return "playback.refactorEditor.history.setKeyframeSubject"; }

SetCameraShake::SetCameraShake(std::string cameraId, std::optional<model::CameraShake> shake)
: mCameraId(std::move(cameraId)),
  mShake(shake) {}

void SetCameraShake::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    std::optional<model::CameraShake> normalized = mShake;
    if (normalized) {
        normalized->startTick         = std::clamp(normalized->startTick, 0, state.totalTicks);
        normalized->endTick           = std::clamp(normalized->endTick, normalized->startTick, state.totalTicks);
        normalized->positionAmplitude = std::max(0.0f, normalized->positionAmplitude);
        normalized->rotationAmplitude = std::max(0.0f, normalized->rotationAmplitude);
        normalized->frequency         = std::clamp(normalized->frequency, 0.01f, 100.0f);
    }
    if (camera->shake == normalized) {
        mBefore.reset();
        return;
    }

    mBefore       = state;
    camera->shake = normalized;
}

void        SetCameraShake::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraShake::label() const { return "playback.refactorEditor.history.setCameraShake"; }

SetCameraKeyframeBezierControls::SetCameraKeyframeBezierControls(
    std::string cameraId,
    int         tick,
    model::Vec2 ctrl1,
    model::Vec2 ctrl2
)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mCtrl1(ctrl1),
  mCtrl2(ctrl2) {}

void SetCameraKeyframeBezierControls::execute(model::EditorStateExt& state) {
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

    model::Vec2 const ctrl1{std::clamp(mCtrl1.x, 0.0f, 1.0f), std::clamp(mCtrl1.y, 0.0f, 1.0f)};
    model::Vec2 const ctrl2{std::clamp(mCtrl2.x, 0.0f, 1.0f), std::clamp(mCtrl2.y, 0.0f, 1.0f)};
    if (key->second.bezierCtrl1.x == ctrl1.x && key->second.bezierCtrl1.y == ctrl1.y
        && key->second.bezierCtrl2.x == ctrl2.x && key->second.bezierCtrl2.y == ctrl2.y) {
        mBefore.reset();
        return;
    }

    mBefore                 = state;
    key->second.bezierCtrl1 = ctrl1;
    key->second.bezierCtrl2 = ctrl2;
}

void        SetCameraKeyframeBezierControls::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeBezierControls::label() const {
    return "playback.refactorEditor.history.setKeyframeBezierControls";
}

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
std::string MoveKeyframes::label() const { return "playback.refactorEditor.history.moveKeyframes"; }

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
std::string DeleteKeyframes::label() const { return "playback.refactorEditor.history.deleteKeyframes"; }

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
std::string SetKeyframesInterpolation::label() const {
    return "playback.refactorEditor.history.setKeyframesInterpolation";
}

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
std::string AddKeyframes::label() const { return "playback.refactorEditor.history.pasteKeyframes"; }

namespace {

std::string
resolveDefaultPersonSubject(model::CameraEntity const& camera, int tick, std::string const& fallbackSubject) {
    auto upper = camera.keysByTick.lower_bound(tick);
    while (upper != camera.keysByTick.begin()) {
        --upper;
        if (upper->second.mode != model::CameraKeyframeMode::Interpolated && !upper->second.subject.empty()) {
            return upper->second.subject;
        }
    }
    return fallbackSubject;
}

} // namespace

SetCameraKeyframeMode::SetCameraKeyframeMode(
    std::string               cameraId,
    int                       tick,
    model::CameraKeyframeMode mode,
    std::string               defaultSubject
)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mMode(mode),
  mDefaultSubject(std::move(defaultSubject)) {}

void SetCameraKeyframeMode::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    int const  tick = std::clamp(mTick, 0, state.totalTicks);
    auto const key  = camera->keysByTick.find(tick);
    if (key == camera->keysByTick.end() || key->second.mode == mMode) {
        mBefore.reset();
        return;
    }

    mBefore          = state;
    key->second.mode = mMode;
    if (mMode != model::CameraKeyframeMode::Interpolated && key->second.subject.empty()) {
        key->second.subject = resolveDefaultPersonSubject(*camera, tick, mDefaultSubject);
    }
}

void        SetCameraKeyframeMode::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeMode::label() const { return "playback.refactorEditor.history.setKeyframeMode"; }

SetKeyframesMode::SetKeyframesMode(
    std::vector<model::KeyframeRef> refs,
    model::CameraKeyframeMode       mode,
    std::string                     defaultSubject
)
: mRefs(std::move(refs)),
  mMode(mode),
  mDefaultSubject(std::move(defaultSubject)) {}

void SetKeyframesMode::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    auto refs    = mRefs;
    std::ranges::sort(refs, [](auto const& a, auto const& b) {
        return std::tie(a.cameraId, a.tick) < std::tie(b.cameraId, b.tick);
    });
    for (auto const& ref : refs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked) continue;
        auto const key = camera->keysByTick.find(ref.tick);
        if (key == camera->keysByTick.end() || key->second.mode == mMode) continue;
        key->second.mode = mMode;
        if (mMode != model::CameraKeyframeMode::Interpolated && key->second.subject.empty()) {
            key->second.subject = resolveDefaultPersonSubject(*camera, ref.tick, mDefaultSubject);
        }
        changed = true;
    }
    if (changed) mBefore = std::move(before);
}

void        SetKeyframesMode::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframesMode::label() const { return "playback.refactorEditor.history.setKeyframesMode"; }

SetKeyframesSubject::SetKeyframesSubject(std::vector<model::KeyframeRef> refs, std::string subject)
: mRefs(std::move(refs)),
  mSubject(std::move(subject)) {}

void SetKeyframesSubject::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    for (auto const& ref : mRefs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked) continue;
        auto const key = camera->keysByTick.find(ref.tick);
        if (key == camera->keysByTick.end()) continue;
        if (key->second.mode == model::CameraKeyframeMode::Interpolated || key->second.subject == mSubject) continue;
        key->second.subject = mSubject;
        changed             = true;
    }
    if (changed) mBefore = std::move(before);
}

void        SetKeyframesSubject::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframesSubject::label() const { return "playback.refactorEditor.history.setKeyframesSubject"; }

MoveCamera::MoveCamera(std::string cameraId, int targetIndex)
: mCameraId(std::move(cameraId)),
  mTargetIndex(targetIndex) {}

void MoveCamera::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const found = std::ranges::find(state.cameras, mCameraId, &model::CameraEntity::id);
    if (found == state.cameras.end()) return;
    auto const from = static_cast<int>(std::distance(state.cameras.begin(), found));
    auto const to   = std::clamp(mTargetIndex, 0, static_cast<int>(state.cameras.size()) - 1);
    if (from == to) return;
    auto before = state;
    if (from < to) std::rotate(found, std::next(found), std::next(state.cameras.begin(), to + 1));
    else std::rotate(std::next(state.cameras.begin(), to), found, std::next(found));
    mBefore = std::move(before);
}

void        MoveCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveCamera::label() const { return "playback.refactorEditor.history.moveCamera"; }

SetCameraKeyframeIgnoreClipSpeed::SetCameraKeyframeIgnoreClipSpeed(std::string cameraId, int tick, bool ignoreClipSpeed)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mIgnoreClipSpeed(ignoreClipSpeed) {}

namespace {

bool supportsIgnoreClipSpeed(model::CameraKeyframe const& key) noexcept {
    return key.mode == model::CameraKeyframeMode::Interpolated
        || (key.mode == model::CameraKeyframeMode::ThirdPerson && key.thirdPersonCustom);
}

float sanitizeFinite(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }

} // namespace

void SetCameraKeyframeIgnoreClipSpeed::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end() || !supportsIgnoreClipSpeed(key->second)
        || key->second.ignoreClipSpeed == mIgnoreClipSpeed) {
        mBefore.reset();
        return;
    }

    mBefore                     = state;
    key->second.ignoreClipSpeed = mIgnoreClipSpeed;
}

void        SetCameraKeyframeIgnoreClipSpeed::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeIgnoreClipSpeed::label() const {
    return "playback.refactorEditor.history.setKeyframeIgnoreClipSpeed";
}

SetKeyframesIgnoreClipSpeed::SetKeyframesIgnoreClipSpeed(std::vector<model::KeyframeRef> refs, bool ignoreClipSpeed)
: mRefs(std::move(refs)),
  mIgnoreClipSpeed(ignoreClipSpeed) {}

void SetKeyframesIgnoreClipSpeed::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    for (auto const& ref : mRefs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked) continue;
        auto const key = camera->keysByTick.find(ref.tick);
        if (key == camera->keysByTick.end() || !supportsIgnoreClipSpeed(key->second)
            || key->second.ignoreClipSpeed == mIgnoreClipSpeed) {
            continue;
        }
        key->second.ignoreClipSpeed = mIgnoreClipSpeed;
        changed                     = true;
    }
    if (changed) mBefore = std::move(before);
}

void        SetKeyframesIgnoreClipSpeed::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframesIgnoreClipSpeed::label() const {
    return "playback.refactorEditor.history.setKeyframesIgnoreClipSpeed";
}

SetCameraKeyframeThirdPersonSettings::SetCameraKeyframeThirdPersonSettings(
    std::string           cameraId,
    int                   tick,
    model::CameraKeyframe settings
)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mSettings(std::move(settings)) {}

void SetCameraKeyframeThirdPersonSettings::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end() || key->second.mode != model::CameraKeyframeMode::ThirdPerson) {
        mBefore.reset();
        return;
    }

    auto&       target         = key->second;
    bool const  custom         = mSettings.thirdPersonCustom;
    float const distance       = std::max(0.0f, sanitizeFinite(mSettings.thirdPersonDistance, target.thirdPersonDistance));
    float const yaw            = sanitizeFinite(mSettings.thirdPersonYaw, target.thirdPersonYaw);
    float const pitch          = sanitizeFinite(mSettings.thirdPersonPitch, target.thirdPersonPitch);
    float const roll           = sanitizeFinite(mSettings.thirdPersonRoll, target.thirdPersonRoll);
    float const fov            = std::clamp(sanitizeFinite(mSettings.thirdPersonFov, target.thirdPersonFov), 1.0f, 179.0f);
    model::Vec3 offset{
        sanitizeFinite(mSettings.thirdPersonOffset.x, target.thirdPersonOffset.x),
        sanitizeFinite(mSettings.thirdPersonOffset.y, target.thirdPersonOffset.y),
        sanitizeFinite(mSettings.thirdPersonOffset.z, target.thirdPersonOffset.z),
    };
    bool const followRotation = mSettings.thirdPersonFollowRotation;
    bool const collideBlocks  = mSettings.thirdPersonCollideBlocks;

    if (target.thirdPersonCustom == custom && target.thirdPersonDistance == distance && target.thirdPersonYaw == yaw
        && target.thirdPersonPitch == pitch && target.thirdPersonRoll == roll && target.thirdPersonFov == fov
        && target.thirdPersonOffset.x == offset.x && target.thirdPersonOffset.y == offset.y
        && target.thirdPersonOffset.z == offset.z && target.thirdPersonFollowRotation == followRotation
        && target.thirdPersonCollideBlocks == collideBlocks) {
        mBefore.reset();
        return;
    }

    mBefore                          = state;
    target.thirdPersonCustom         = custom;
    target.thirdPersonDistance       = distance;
    target.thirdPersonYaw            = yaw;
    target.thirdPersonPitch          = pitch;
    target.thirdPersonRoll           = roll;
    target.thirdPersonFov            = fov;
    target.thirdPersonOffset         = offset;
    target.thirdPersonFollowRotation = followRotation;
    target.thirdPersonCollideBlocks  = collideBlocks;
}

void        SetCameraKeyframeThirdPersonSettings::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeThirdPersonSettings::label() const {
    return "playback.refactorEditor.history.setKeyframeThirdPersonSettings";
}

SetKeyframesThirdPersonCustom::SetKeyframesThirdPersonCustom(std::vector<model::KeyframeRef> refs, bool custom)
: mRefs(std::move(refs)),
  mCustom(custom) {}

void SetKeyframesThirdPersonCustom::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    for (auto const& ref : mRefs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked) continue;
        auto const key = camera->keysByTick.find(ref.tick);
        if (key == camera->keysByTick.end() || key->second.mode != model::CameraKeyframeMode::ThirdPerson
            || key->second.thirdPersonCustom == mCustom) {
            continue;
        }
        key->second.thirdPersonCustom = mCustom;
        changed                       = true;
    }
    if (changed) mBefore = std::move(before);
}

void        SetKeyframesThirdPersonCustom::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframesThirdPersonCustom::label() const {
    return "playback.refactorEditor.history.setKeyframesThirdPersonMode";
}

SetCameraKeyframeLookAt::SetCameraKeyframeLookAt(
    std::string cameraId,
    int         tick,
    bool        enabled,
    std::string target,
    float       offsetY
)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mEnabled(enabled),
  mTarget(std::move(target)),
  mOffsetY(offsetY) {}

void SetCameraKeyframeLookAt::execute(model::EditorStateExt& state) {
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked) {
        mBefore.reset();
        return;
    }

    auto const key = camera->keysByTick.find(std::clamp(mTick, 0, state.totalTicks));
    if (key == camera->keysByTick.end() || key->second.mode != model::CameraKeyframeMode::Interpolated) {
        mBefore.reset();
        return;
    }

    float const offsetY = sanitizeFinite(mOffsetY, key->second.lookAtOffsetY);
    if (key->second.lookAtEnabled == mEnabled && key->second.lookAtTarget == mTarget
        && key->second.lookAtOffsetY == offsetY) {
        mBefore.reset();
        return;
    }

    mBefore                   = state;
    key->second.lookAtEnabled = mEnabled;
    key->second.lookAtTarget  = mTarget;
    key->second.lookAtOffsetY = offsetY;
}

void        SetCameraKeyframeLookAt::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetCameraKeyframeLookAt::label() const { return "playback.refactorEditor.history.setKeyframeLookAt"; }

SetKeyframesLookAt::SetKeyframesLookAt(
    std::vector<model::KeyframeRef> refs,
    bool                            enabled,
    std::string                     target,
    std::optional<float>            offsetY
)
: mRefs(std::move(refs)),
  mEnabled(enabled),
  mTarget(std::move(target)),
  mOffsetY(offsetY) {}

void SetKeyframesLookAt::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto before  = state;
    bool changed = false;
    for (auto const& ref : mRefs) {
        auto* camera = findCamera(state, ref.cameraId);
        if (!camera || camera->locked) continue;
        auto const key = camera->keysByTick.find(ref.tick);
        if (key == camera->keysByTick.end() || key->second.mode != model::CameraKeyframeMode::Interpolated) continue;
        float const nextOffsetY =
            mOffsetY ? sanitizeFinite(*mOffsetY, key->second.lookAtOffsetY) : key->second.lookAtOffsetY;
        if (key->second.lookAtEnabled == mEnabled && key->second.lookAtTarget == mTarget
            && key->second.lookAtOffsetY == nextOffsetY) {
            continue;
        }
        key->second.lookAtEnabled = mEnabled;
        key->second.lookAtTarget  = mTarget;
        key->second.lookAtOffsetY = nextOffsetY;
        changed                   = true;
    }
    if (changed) mBefore = std::move(before);
}

void        SetKeyframesLookAt::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetKeyframesLookAt::label() const { return "playback.refactorEditor.history.setKeyframesLookAt"; }

UpdateCameraKeyframePose::UpdateCameraKeyframePose(std::string cameraId, int tick, model::CameraKeyframe pose)
: mCameraId(std::move(cameraId)),
  mTick(tick),
  mPose(std::move(pose)) {}

void UpdateCameraKeyframePose::execute(model::EditorStateExt& state) {
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

    auto& target = key->second;
    if (target.mode == model::CameraKeyframeMode::Interpolated) {
        model::Vec3 const pos{
            sanitizeFinite(mPose.position.x, target.position.x),
            sanitizeFinite(mPose.position.y, target.position.y),
            sanitizeFinite(mPose.position.z, target.position.z),
        };
        float const yaw   = sanitizeFinite(mPose.yaw, target.yaw);
        float const pitch = sanitizeFinite(mPose.pitch, target.pitch);
        float const roll  = sanitizeFinite(mPose.roll, target.roll);
        float const fov   = std::clamp(sanitizeFinite(mPose.fov, target.fov), 1.0f, 179.0f);
        if (target.position.x == pos.x && target.position.y == pos.y && target.position.z == pos.z
            && target.yaw == yaw && target.pitch == pitch && target.roll == roll && target.fov == fov) {
            mBefore.reset();
            return;
        }
        mBefore         = state;
        target.position = pos;
        target.yaw      = yaw;
        target.pitch    = pitch;
        target.roll     = roll;
        target.fov      = fov;
        return;
    }

    if (target.mode == model::CameraKeyframeMode::ThirdPerson && target.thirdPersonCustom) {
        model::Vec3 const offset{
            sanitizeFinite(mPose.thirdPersonOffset.x, target.thirdPersonOffset.x),
            sanitizeFinite(mPose.thirdPersonOffset.y, target.thirdPersonOffset.y),
            sanitizeFinite(mPose.thirdPersonOffset.z, target.thirdPersonOffset.z),
        };
        float const distance = std::max(0.0f, sanitizeFinite(mPose.thirdPersonDistance, target.thirdPersonDistance));
        float const yaw      = sanitizeFinite(mPose.thirdPersonYaw, target.thirdPersonYaw);
        float const pitch    = sanitizeFinite(mPose.thirdPersonPitch, target.thirdPersonPitch);
        float const roll     = sanitizeFinite(mPose.thirdPersonRoll, target.thirdPersonRoll);
        float const fov      = std::clamp(sanitizeFinite(mPose.thirdPersonFov, target.thirdPersonFov), 1.0f, 179.0f);
        if (target.thirdPersonOffset.x == offset.x && target.thirdPersonOffset.y == offset.y
            && target.thirdPersonOffset.z == offset.z && target.thirdPersonDistance == distance
            && target.thirdPersonYaw == yaw && target.thirdPersonPitch == pitch && target.thirdPersonRoll == roll
            && target.thirdPersonFov == fov) {
            mBefore.reset();
            return;
        }
        mBefore                    = state;
        target.thirdPersonOffset   = offset;
        target.thirdPersonDistance = distance;
        target.thirdPersonYaw      = yaw;
        target.thirdPersonPitch    = pitch;
        target.thirdPersonRoll     = roll;
        target.thirdPersonFov      = fov;
        return;
    }

    mBefore.reset();
}

void        UpdateCameraKeyframePose::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string UpdateCameraKeyframePose::label() const { return "playback.refactorEditor.history.captureKeyframePose"; }

} // namespace playback::state::editing::command
