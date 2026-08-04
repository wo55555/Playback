#include "CameraCommands.h"

#include "playback/editor/editing/CameraBindingOps.h"

#include <algorithm>
#include <cmath>
namespace playback::editor::editing::command {
namespace { model::CameraEntity* camera(model::EditorStateExt&s,const std::string&id){auto it=std::find_if(s.cameras.begin(),s.cameras.end(),[&](const auto&v){return v.id==id;});return it==s.cameras.end()?nullptr:&*it;} void restore(std::optional<model::EditorStateExt>&before,model::EditorStateExt&s){if(before)s=*before;} }
AddFreeCamera::AddFreeCamera(std::string name):mName(std::move(name)){} void AddFreeCamera::execute(model::EditorStateExt&s){mBefore=s;CameraBindingOps::addFreeCamera(s,mName);}void AddFreeCamera::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string AddFreeCamera::label()const{return "Add Free Camera";}
DeleteCamera::DeleteCamera(std::string id):mId(std::move(id)){}void DeleteCamera::execute(model::EditorStateExt&s){mBefore=s;CameraBindingOps::deleteCamera(s,mId);}void DeleteCamera::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string DeleteCamera::label()const{return "Delete Camera";}
CreateBindingCamera::CreateBindingCamera(std::string id,std::string name):mSubActorId(std::move(id)),mName(std::move(name)){}void CreateBindingCamera::execute(model::EditorStateExt&s){mBefore=s;CameraBindingOps::createBindingCamera(s,mSubActorId,mName);}void CreateBindingCamera::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string CreateBindingCamera::label()const{return "Create Binding Camera";}
UnbindCamera::UnbindCamera(std::string id):mId(std::move(id)){}void UnbindCamera::execute(model::EditorStateExt&s){mBefore=s;CameraBindingOps::unbindCamera(s,mId);}void UnbindCamera::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string UnbindCamera::label()const{return "Unbind Camera";}
AddKeyframe::AddKeyframe(std::string id,int tick):mCameraId(std::move(id)),mTick(tick){}void AddKeyframe::execute(model::EditorStateExt&s){mBefore=s;auto*c=camera(s,mCameraId);int tick=std::clamp(mTick,0,s.totalTicks);if(!c||c->locked||std::any_of(c->keys.begin(),c->keys.end(),[&](const auto&k){return k.tick==tick;}))return;model::CameraKeyframe key;key.id=mCameraId+".key."+std::to_string(tick);key.tick=tick;if(!c->keys.empty())key=c->keys.back(),key.id=mCameraId+".key."+std::to_string(tick),key.tick=tick;c->keys.push_back(key);std::sort(c->keys.begin(),c->keys.end(),[](const auto&a,const auto&b){return a.tick<b.tick;});}void AddKeyframe::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string AddKeyframe::label()const{return "Add Keyframe";}
CaptureCameraKeyframe::CaptureCameraKeyframe(std::string id,int tick,model::Vec3 position,float yaw,float pitch,float fov):mCameraId(std::move(id)),mTick(tick),mPosition(position),mYaw(yaw),mPitch(pitch),mFov(fov){}void CaptureCameraKeyframe::execute(model::EditorStateExt&s){mBefore=s;auto*c=camera(s,mCameraId);const int tick=std::clamp(mTick,0,s.totalTicks);if(!c||c->locked)return;auto it=std::find_if(c->keys.begin(),c->keys.end(),[&](const auto&k){return k.tick==tick;});if(it==c->keys.end()){model::CameraKeyframe key;key.id=mCameraId+".key."+std::to_string(tick);key.tick=tick;c->keys.push_back(key);it=std::prev(c->keys.end());}it->position=mPosition;it->yaw=mYaw;it->pitch=mPitch;it->fov=mFov;std::sort(c->keys.begin(),c->keys.end(),[](const auto&a,const auto&b){return a.tick<b.tick;});}void CaptureCameraKeyframe::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string CaptureCameraKeyframe::label()const{return "Capture Camera Keyframe";}
MoveKeyframe::MoveKeyframe(std::string c,std::string k,int tick):mCameraId(std::move(c)),mKeyframeId(std::move(k)),mTick(tick){}void MoveKeyframe::execute(model::EditorStateExt&s){mBefore=s;auto*c=camera(s,mCameraId);if(!c||c->locked)return;auto it=std::find_if(c->keys.begin(),c->keys.end(),[&](const auto&k){return k.id==mKeyframeId;});int targetTick=std::clamp(mTick,0,s.totalTicks);if(it==c->keys.end()||std::any_of(c->keys.begin(),c->keys.end(),[&](const auto&k){return k.id!=mKeyframeId&&k.tick==targetTick;}))return;it->tick=targetTick;std::sort(c->keys.begin(),c->keys.end(),[](const auto&a,const auto&b){return a.tick<b.tick;});}void MoveKeyframe::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string MoveKeyframe::label()const{return "Move Keyframe";}
DeleteKeyframe::DeleteKeyframe(std::string c,std::string k):mCameraId(std::move(c)),mKeyframeId(std::move(k)){}void DeleteKeyframe::execute(model::EditorStateExt&s){mBefore=s;auto*c=camera(s,mCameraId);if(!c||c->locked)return;c->keys.erase(std::remove_if(c->keys.begin(),c->keys.end(),[&](const auto&k){return k.id==mKeyframeId;}),c->keys.end());}void DeleteKeyframe::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string DeleteKeyframe::label()const{return "Delete Keyframe";}
SetKeyframeEasing::SetKeyframeEasing(std::string c,std::string k,model::EasingType e):mCameraId(std::move(c)),mKeyframeId(std::move(k)),mEasing(e){}void SetKeyframeEasing::execute(model::EditorStateExt&s){mBefore=s;auto*c=camera(s,mCameraId);if(!c||c->locked)return;auto it=std::find_if(c->keys.begin(),c->keys.end(),[&](const auto&k){return k.id==mKeyframeId;});if(it!=c->keys.end())it->easingType=mEasing;}void SetKeyframeEasing::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string SetKeyframeEasing::label()const{return "Set Keyframe Easing";}
ApplyCameraTransitionPreset::ApplyCameraTransitionPreset(std::string c,std::string k,model::CameraTransitionPreset preset):mCameraId(std::move(c)),mKeyframeId(std::move(k)),mPreset(preset){}void ApplyCameraTransitionPreset::execute(model::EditorStateExt&s){mBefore=s;auto*c=camera(s,mCameraId);if(!c||c->locked)return;auto it=std::find_if(c->keys.begin(),c->keys.end(),[&](const auto&k){return k.id==mKeyframeId;});if(it==c->keys.end()||std::next(it)==c->keys.end())return;auto&motion=it->outgoingMotion;motion={};motion.preset=mPreset;switch(mPreset){case model::CameraTransitionPreset::CinematicEase:it->easingType=model::EasingType::EaseInOut;break;case model::CameraTransitionPreset::ArcPushIn:motion.pathType=model::CameraPathType::CubicBezier;it->easingType=model::EasingType::EaseInOut;motion.outControl={0,2,0};motion.inControl={0,1,0};motion.fovPeakOffset=-8.0f;break;case model::CameraTransitionPreset::ArcPullOut:motion.pathType=model::CameraPathType::CubicBezier;it->easingType=model::EasingType::EaseInOut;motion.outControl={0,2,0};motion.inControl={0,1,0};motion.fovPeakOffset=8.0f;break;case model::CameraTransitionPreset::OrbitPass:motion.pathType=model::CameraPathType::CubicBezier;it->easingType=model::EasingType::CubicBezier;motion.useLookAlongPath=true;motion.outControl={2,0,0};motion.inControl={-2,0,0};break;case model::CameraTransitionPreset::WhipPan:it->easingType=model::EasingType::EaseOut;motion.fovPeakOffset=3.0f;break;case model::CameraTransitionPreset::ZoomTransition:it->easingType=model::EasingType::EaseInOut;motion.fovPeakOffset=-15.0f;break;case model::CameraTransitionPreset::Custom:case model::CameraTransitionPreset::LinearConstant:break;}}void ApplyCameraTransitionPreset::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string ApplyCameraTransitionPreset::label()const{return "Apply Camera Transition Preset";}
SetCameraKind::SetCameraKind(std::string id,model::CameraKind kind):mCameraId(std::move(id)),mKind(kind){}void SetCameraKind::execute(model::EditorStateExt&s){mBefore=s;auto*c=camera(s,mCameraId);if(c&&!c->locked)c->kind=mKind;}void SetCameraKind::undo(model::EditorStateExt&s){restore(mBefore,s);}std::string SetCameraKind::label()const{return "Set Camera Kind";}
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
    if (!camera->keys.empty()) key = camera->keys.back();
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

SetCameraKind::SetCameraKind(std::string cameraId, model::CameraKind kind)
: mCameraId(std::move(cameraId)),
  mKind(kind) {}

void SetCameraKind::execute(model::EditorStateExt& state) {
    mChanged     = false;
    auto* camera = findCamera(state, mCameraId);
    if (!camera || camera->locked || camera->kind == mKind) {
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
