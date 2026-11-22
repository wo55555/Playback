#include "ClipCommands.h"

#include "ll/api/i18n/I18n.h"
#include "playback/state/editing/CameraBindingOps.h"
#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace playback::state::editing::command {
using namespace ll::i18n_literals;
namespace {

struct TickGap {
    int start{};
    int end{};
};

struct ClipLocation {
    size_t cameraIdx{};
    size_t clipIdx{};
};

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

std::optional<size_t> findCameraIndex(model::EditorStateExt const& state, std::string_view cameraId) {
    if (cameraId.empty()) return std::nullopt;
    for (size_t i = 0; i < state.cameras.size(); ++i) {
        if (state.cameras[i].id == cameraId) return i;
    }
    return std::nullopt;
}

std::optional<ClipLocation> findClipLocation(model::EditorStateExt const& state, std::string_view clipId) {
    if (clipId.empty()) return std::nullopt;
    for (size_t camIdx = 0; camIdx < state.cameras.size(); ++camIdx) {
        auto const& clips = state.cameras[camIdx].clips;
        for (size_t clipIdx = 0; clipIdx < clips.size(); ++clipIdx) {
            if (clips[clipIdx].id == clipId) return ClipLocation{camIdx, clipIdx};
        }
    }
    return std::nullopt;
}

std::optional<size_t> findSceneIndex(model::EditorStateExt const& state, std::string const& sceneId) {
    std::string const& target = sceneId.empty() ? state.activeSceneId : sceneId;
    if (target.empty()) return std::nullopt;
    for (size_t i = 0; i < state.scenes.size(); ++i) {
        if (state.scenes[i].id == target) return i;
    }
    return std::nullopt;
}

void sortClips(std::vector<model::Clip>& clips) { std::ranges::stable_sort(clips, {}, &model::Clip::inTick); }

bool isFullRangeDefault(model::CameraEntity const& camera, int totalTicks) noexcept {
    return camera.clips.size() == 1 && camera.clips[0].inTick == 0 && camera.clips[0].outTick == totalTicks
        && std::abs(camera.clips[0].speed - 1.0f) < 1e-6f && !camera.clips[0].muted && camera.clips[0].exportEnabled;
}

std::string defaultSceneName(model::EditorStateExt const& state) {
    for (size_t next = state.scenes.size() + 1;; ++next) {
        auto candidate = "playback.refactorEditor.defaults.scene"_tr(next);
        if (std::ranges::none_of(state.scenes, [&](auto const& scene) { return scene.name == candidate; })) {
            return candidate;
        }
    }
}

std::vector<TickGap> collectFreeGaps(std::vector<model::Clip> const& clips, int totalTicks) {
    std::vector<model::Clip const*> sorted;
    sorted.reserve(clips.size());
    for (auto const& clip : clips) sorted.push_back(&clip);
    std::ranges::stable_sort(sorted, [](auto const* a, auto const* b) { return a->inTick < b->inTick; });

    std::vector<TickGap> gaps;
    int                  cursor = 0;
    for (auto const* clip : sorted) {
        int const cIn  = std::clamp(clip->inTick, 0, totalTicks);
        int const cOut = std::clamp(clip->outTick, cIn, totalTicks);
        if (cIn > cursor) gaps.push_back({cursor, cIn});
        cursor = std::max(cursor, cOut);
    }
    if (cursor < totalTicks) gaps.push_back({cursor, totalTicks});
    return gaps;
}

} // namespace

AddClip::AddClip(std::string cameraId, int inTick, int outTick, std::string name, float speed)
: mCameraId(std::move(cameraId)),
  mInTick(inTick),
  mOutTick(outTick),
  mName(std::move(name)),
  mSpeed(speed) {}

void AddClip::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.totalTicks <= 0 || state.cameras.empty()) return;

    std::optional<size_t> camIdx = mCameraId.empty() ? std::optional<size_t>{0} : findCameraIndex(state, mCameraId);
    if (!camIdx || state.cameras[*camIdx].locked) return;

    auto const gaps = collectFreeGaps(state.cameras[*camIdx].clips, state.totalTicks);
    if (gaps.empty()) return;

    int const reqIn      = std::clamp(mInTick, 0, state.totalTicks - 1);
    int const desiredLen = std::max(1, mOutTick - mInTick);
    int const reqOut     = std::clamp(reqIn + desiredLen, reqIn + 1, state.totalTicks);

    int finalIn  = -1;
    int finalOut = -1;
    for (auto const& gap : gaps) {
        if (gap.start <= reqIn && reqIn < gap.end) {
            finalIn  = reqIn;
            finalOut = std::min(reqOut, gap.end);
            break;
        }
    }
    if (finalIn < 0) {
        for (auto const& gap : gaps) {
            if (gap.end - gap.start >= 1) {
                finalIn  = gap.start;
                finalOut = std::min(gap.end, gap.start + desiredLen);
                break;
            }
        }
    }
    if (finalIn < 0 || finalOut <= finalIn) return;

    auto const newId   = ClipOps::nextClipId(state);
    auto const newName = mName.empty() ? ClipOps::defaultClipName(state) : mName;
    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    model::Clip clip;
    clip.id            = newId;
    clip.inTick        = finalIn;
    clip.outTick       = finalOut;
    clip.speed         = model::clampClipSpeed(mSpeed);
    clip.name          = newName;
    clip.exportEnabled = true;

    auto& clips = state.cameras[*camIdx].clips;
    clips.push_back(std::move(clip));
    sortClips(clips);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddClip::label() const { return "playback.refactorEditor.history.addClip"; }

SplitClip::SplitClip(std::string clipId, int splitTick, std::string newClipName)
: mClipId(std::move(clipId)),
  mSplitTick(splitTick),
  mNewClipName(std::move(newClipName)) {}

void SplitClip::execute(model::EditorStateExt& state) {
    mBefore.reset();

    std::optional<ClipLocation> loc;
    if (mClipId.empty()) {
        for (size_t camIdx = 0; camIdx < state.cameras.size() && !loc; ++camIdx) {
            auto const& cam = state.cameras[camIdx];
            if (!cam.enabled || cam.locked) continue;
            for (size_t clipIdx = 0; clipIdx < cam.clips.size(); ++clipIdx) {
                auto const& clip = cam.clips[clipIdx];
                if (!clip.locked && clip.inTick < mSplitTick && mSplitTick < clip.outTick) {
                    loc = ClipLocation{camIdx, clipIdx};
                    break;
                }
            }
        }
    } else {
        loc = findClipLocation(state, mClipId);
    }

    if (!loc) return;
    auto const& cam  = state.cameras[loc->cameraIdx];
    auto const& left = cam.clips[loc->clipIdx];
    if (cam.locked || left.locked || mSplitTick <= left.inTick || mSplitTick >= left.outTick) return;

    auto const newId   = ClipOps::nextClipId(state);
    auto const newName = mNewClipName.empty() ? ClipOps::defaultClipName(state) : mNewClipName;
    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    auto&       clips   = state.cameras[loc->cameraIdx].clips;
    auto&       leftRef = clips[loc->clipIdx];
    model::Clip right;
    right.id            = newId;
    right.replayFile    = leftRef.replayFile;
    right.inTick        = mSplitTick;
    right.outTick       = leftRef.outTick;
    right.trackTick     = mSplitTick;
    right.speed         = leftRef.speed;
    right.name          = newName;
    right.color         = leftRef.color;
    right.muted         = leftRef.muted;
    right.locked        = false;
    right.exportEnabled = leftRef.exportEnabled;

    leftRef.outTick = mSplitTick;
    clips.insert(clips.begin() + static_cast<std::ptrdiff_t>(loc->clipIdx + 1), std::move(right));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SplitClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SplitClip::label() const { return "playback.refactorEditor.history.splitClip"; }

MoveAndTrimClip::MoveAndTrimClip(std::string clipId, int newInTick, int newOutTick)
: mClipId(std::move(clipId)),
  mNewInTick(newInTick),
  mNewOutTick(newOutTick) {}

void MoveAndTrimClip::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.totalTicks <= 0) return;

    auto const loc = findClipLocation(state, mClipId);
    if (!loc) return;
    auto const& cam  = state.cameras[loc->cameraIdx];
    auto const& clip = cam.clips[loc->clipIdx];
    if (cam.locked || clip.locked) return;

    bool const inBounds      = mNewInTick >= 0 && mNewOutTick > mNewInTick && mNewOutTick <= state.totalTicks;
    bool       overlapsOther = false;
    if (inBounds) {
        for (size_t i = 0; i < cam.clips.size(); ++i) {
            if (i == loc->clipIdx) continue;
            auto const& other = cam.clips[i];
            if (other.inTick < mNewOutTick && other.outTick > mNewInTick) {
                overlapsOther = true;
                break;
            }
        }
    }

    int clampedIn  = 0;
    int clampedOut = 0;
    if (inBounds && !overlapsOther) {
        clampedIn  = mNewInTick;
        clampedOut = mNewOutTick;
    } else {
        int minIn  = 0;
        int maxOut = state.totalTicks;
        for (size_t i = 0; i < cam.clips.size(); ++i) {
            if (i == loc->clipIdx) continue;
            auto const& other = cam.clips[i];
            if (other.outTick <= clip.inTick) minIn = std::max(minIn, other.outTick);
            if (other.inTick >= clip.outTick) maxOut = std::min(maxOut, other.inTick);
        }
        minIn  = std::clamp(minIn, 0, state.totalTicks);
        maxOut = std::clamp(maxOut, minIn, state.totalTicks);

        clampedIn  = std::clamp(mNewInTick, minIn, maxOut);
        clampedOut = std::clamp(mNewOutTick, minIn, maxOut);
    }
    if (clampedOut <= clampedIn || (clampedIn == clip.inTick && clampedOut == clip.outTick)) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                     = state;
    auto& clips                 = state.cameras[loc->cameraIdx].clips;
    clips[loc->clipIdx].inTick  = clampedIn;
    clips[loc->clipIdx].outTick = clampedOut;
    sortClips(clips);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveAndTrimClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveAndTrimClip::label() const { return "playback.refactorEditor.history.moveOrTrimClip"; }

SetClipSpeed::SetClipSpeed(std::string clipId, float speed) : mClipId(std::move(clipId)), mSpeed(speed) {}

void SetClipSpeed::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const loc = findClipLocation(state, mClipId);
    if (!loc) return;
    auto const& cam  = state.cameras[loc->cameraIdx];
    auto const& clip = cam.clips[loc->clipIdx];
    if (cam.locked || clip.locked) return;

    float const clamped = model::clampClipSpeed(mSpeed);
    if (std::abs(clip.speed - clamped) < 1e-6f) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                                                 = state;
    state.cameras[loc->cameraIdx].clips[loc->clipIdx].speed = clamped;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetClipSpeed::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetClipSpeed::label() const { return "playback.refactorEditor.history.setClipSpeed"; }

RenameClip::RenameClip(std::string clipId, std::string name) : mClipId(std::move(clipId)), mName(std::move(name)) {}

void RenameClip::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const loc = findClipLocation(state, mClipId);
    if (!loc) return;
    auto const& cam  = state.cameras[loc->cameraIdx];
    auto const& clip = cam.clips[loc->clipIdx];
    if (cam.locked || clip.locked || mName.empty() || clip.name == mName) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                                                = state;
    state.cameras[loc->cameraIdx].clips[loc->clipIdx].name = mName;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RenameClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RenameClip::label() const { return "playback.refactorEditor.history.renameClip"; }

SetClipMuted::SetClipMuted(std::string clipId, bool muted) : mClipId(std::move(clipId)), mMuted(muted) {}

void SetClipMuted::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const loc = findClipLocation(state, mClipId);
    if (!loc) return;
    auto const& cam  = state.cameras[loc->cameraIdx];
    auto const& clip = cam.clips[loc->clipIdx];
    if (cam.locked || clip.locked || clip.muted == mMuted) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                                                 = state;
    state.cameras[loc->cameraIdx].clips[loc->clipIdx].muted = mMuted;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetClipMuted::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetClipMuted::label() const { return "playback.refactorEditor.history.setClipMuted"; }

SetClipLocked::SetClipLocked(std::string clipId, bool locked) : mClipId(std::move(clipId)), mLocked(locked) {}

void SetClipLocked::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const loc = findClipLocation(state, mClipId);
    if (!loc) return;
    auto const& cam  = state.cameras[loc->cameraIdx];
    auto const& clip = cam.clips[loc->clipIdx];
    if (cam.locked || clip.locked == mLocked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                                                  = state;
    state.cameras[loc->cameraIdx].clips[loc->clipIdx].locked = mLocked;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetClipLocked::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetClipLocked::label() const { return "playback.refactorEditor.history.setClipLocked"; }

SetClipExportEnabled::SetClipExportEnabled(std::string clipId, bool exportEnabled)
: mClipId(std::move(clipId)),
  mExportEnabled(exportEnabled) {}

void SetClipExportEnabled::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const loc = findClipLocation(state, mClipId);
    if (!loc) return;
    auto const& cam  = state.cameras[loc->cameraIdx];
    auto const& clip = cam.clips[loc->clipIdx];
    if (cam.locked || clip.exportEnabled == mExportEnabled) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                                                         = state;
    state.cameras[loc->cameraIdx].clips[loc->clipIdx].exportEnabled = mExportEnabled;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetClipExportEnabled::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetClipExportEnabled::label() const { return "playback.refactorEditor.history.setClipExportEnabled"; }

SetAllClipsExportEnabled::SetAllClipsExportEnabled(bool exportEnabled, std::string cameraId)
: mExportEnabled(exportEnabled),
  mCameraId(std::move(cameraId)) {}

void SetAllClipsExportEnabled::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!mCameraId.empty()) {
        auto const camIdx = findCameraIndex(state, mCameraId);
        if (!camIdx) return;
        auto const& cam = state.cameras[*camIdx];
        if (cam.locked || cam.clips.empty()) return;
        if (std::ranges::all_of(cam.clips, [&](auto const& clip) { return clip.exportEnabled == mExportEnabled; })) {
            return;
        }

        ClipOps::syncActiveSceneFromWorking(state);
        mBefore = state;
        for (auto& clip : state.cameras[*camIdx].clips) clip.exportEnabled = mExportEnabled;
        ClipOps::syncActiveSceneFromWorking(state);
        return;
    }

    bool anyChanged = false;
    for (auto const& cam : state.cameras) {
        if (cam.locked) continue;
        if (std::ranges::any_of(cam.clips, [&](auto const& clip) { return clip.exportEnabled != mExportEnabled; })) {
            anyChanged = true;
            break;
        }
    }
    if (!anyChanged) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    for (auto& cam : state.cameras) {
        if (cam.locked) continue;
        for (auto& clip : cam.clips) clip.exportEnabled = mExportEnabled;
    }
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetAllClipsExportEnabled::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetAllClipsExportEnabled::label() const {
    return "playback.refactorEditor.history.setAllClipsExportEnabled";
}

ResetClipsToFullRange::ResetClipsToFullRange(std::string cameraId, std::string defaultClipName)
: mCameraId(std::move(cameraId)),
  mDefaultClipName(std::move(defaultClipName)) {}

void ResetClipsToFullRange::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.totalTicks <= 0) return;

    if (!mCameraId.empty()) {
        auto const camIdx = findCameraIndex(state, mCameraId);
        if (!camIdx) return;
        auto const& cam = state.cameras[*camIdx];
        if (cam.locked || std::ranges::any_of(cam.clips, &model::Clip::locked)) return;
        if (isFullRangeDefault(cam, state.totalTicks)) return;

        ClipOps::syncActiveSceneFromWorking(state);
        mBefore = state;
        state.cameras[*camIdx].clips.clear();
        ClipOps::syncActiveSceneFromWorking(state);

        model::Clip clip;
        clip.id            = ClipOps::nextClipId(state);
        clip.name          = mDefaultClipName.empty() ? ClipOps::defaultClipName(state) : mDefaultClipName;
        clip.inTick        = 0;
        clip.outTick       = state.totalTicks;
        clip.speed         = 1.0f;
        clip.exportEnabled = true;
        state.cameras[*camIdx].clips.push_back(std::move(clip));
        ClipOps::syncActiveSceneFromWorking(state);
        return;
    }

    if (state.cameras.empty()) return;
    if (std::ranges::any_of(state.cameras, [](auto const& cam) {
            return cam.locked || std::ranges::any_of(cam.clips, &model::Clip::locked);
        })) {
        return;
    }
    if (std::ranges::all_of(state.cameras, [&](auto const& cam) {
            return isFullRangeDefault(cam, state.totalTicks);
        })) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    for (auto& cam : state.cameras) cam.clips.clear();
    ClipOps::syncActiveSceneFromWorking(state);

    for (size_t i = 0; i < state.cameras.size(); ++i) {
        model::Clip clip;
        clip.id            = ClipOps::nextClipId(state);
        clip.name          = (i == 0 && !mDefaultClipName.empty()) ? mDefaultClipName : ClipOps::defaultClipName(state);
        clip.inTick        = 0;
        clip.outTick       = state.totalTicks;
        clip.speed         = 1.0f;
        clip.exportEnabled = true;
        state.cameras[i].clips.push_back(std::move(clip));
        ClipOps::syncActiveSceneFromWorking(state);
    }
}

void        ResetClipsToFullRange::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string ResetClipsToFullRange::label() const { return "playback.refactorEditor.history.resetClipsToFullRange"; }

DeleteClip::DeleteClip(std::string clipId) : mClipId(std::move(clipId)) {}

void DeleteClip::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const loc = findClipLocation(state, mClipId);
    if (!loc) return;
    auto const& cam = state.cameras[loc->cameraIdx];
    if (cam.locked || cam.clips[loc->clipIdx].locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore     = state;
    auto& clips = state.cameras[loc->cameraIdx].clips;
    clips.erase(clips.begin() + static_cast<std::ptrdiff_t>(loc->clipIdx));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        DeleteClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteClip::label() const { return "playback.refactorEditor.history.deleteClip"; }

AddScene::AddScene(std::string name) : mName(std::move(name)) {}

void AddScene::execute(model::EditorStateExt& state) {
    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    if (state.scenes.empty()) ClipOps::ensureDefaultScene(state);

    model::Scene scene;
    scene.id   = ClipOps::nextSceneId(state);
    scene.name = mName.empty() ? defaultSceneName(state) : mName;

    model::EditorStateExt temp;
    CameraBindingOps::addFreeCamera(temp, {});
    scene.cameras = std::move(temp.cameras);

    state.scenes.push_back(std::move(scene));
    state.activeSceneId = state.scenes.back().id;
    if (state.totalTicks > 0 && !state.scenes.back().cameras.empty()) {
        model::Clip clip;
        clip.id            = ClipOps::nextClipId(state);
        clip.name          = ClipOps::defaultClipName(state);
        clip.inTick        = 0;
        clip.outTick       = state.totalTicks;
        clip.speed         = 1.0f;
        clip.exportEnabled = true;
        state.scenes.back().cameras.front().clips.push_back(std::move(clip));
    }

    ClipOps::loadWorkingFromActiveScene(state);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddScene::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddScene::label() const { return "playback.refactorEditor.history.addScene"; }

DuplicateScene::DuplicateScene(std::string sceneId, std::string copyName)
: mSceneId(std::move(sceneId)),
  mCopyName(std::move(copyName)) {}

void DuplicateScene::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const srcIdx = findSceneIndex(state, mSceneId);
    if (!srcIdx) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    auto const&  src  = state.scenes[*srcIdx];
    model::Scene copy = src;
    copy.id           = ClipOps::nextSceneId(state);
    copy.name         = !mCopyName.empty() ? mCopyName
                      : !src.name.empty()  ? "playback.refactorEditor.defaults.sceneCopy"_tr(src.name)
                                           : defaultSceneName(state);

    auto const insertPos = state.scenes.begin() + static_cast<std::ptrdiff_t>(*srcIdx + 1);
    auto const inserted  = state.scenes.insert(insertPos, std::move(copy));
    for (auto& cam : inserted->cameras) {
        for (auto& clip : cam.clips) {
            clip.id = ClipOps::nextClipId(state);
        }
    }
    state.activeSceneId = inserted->id;
    ClipOps::loadWorkingFromActiveScene(state);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        DuplicateScene::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DuplicateScene::label() const { return "playback.refactorEditor.history.duplicateScene"; }

RenameScene::RenameScene(std::string sceneId, std::string name)
: mSceneId(std::move(sceneId)),
  mName(std::move(name)) {}

void RenameScene::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (mName.empty()) return;
    auto const idx = findSceneIndex(state, mSceneId);
    if (!idx || state.scenes[*idx].name == mName) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                 = state;
    state.scenes[*idx].name = mName;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RenameScene::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RenameScene::label() const { return "playback.refactorEditor.history.renameScene"; }

MoveScene::MoveScene(std::string sceneId, int targetIndex) : mSceneId(std::move(sceneId)), mTargetIndex(targetIndex) {}

void MoveScene::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.scenes.size() <= 1) return;
    auto const fromIdx = findSceneIndex(state, mSceneId);
    if (!fromIdx) return;

    int const clamped = std::clamp(mTargetIndex, 0, static_cast<int>(state.scenes.size()) - 1);
    if (clamped == static_cast<int>(*fromIdx)) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    auto const first = state.scenes.begin();
    if (static_cast<int>(*fromIdx) < clamped) {
        std::rotate(
            first + static_cast<std::ptrdiff_t>(*fromIdx),
            first + static_cast<std::ptrdiff_t>(*fromIdx + 1),
            first + static_cast<std::ptrdiff_t>(clamped + 1)
        );
    } else {
        std::rotate(
            first + static_cast<std::ptrdiff_t>(clamped),
            first + static_cast<std::ptrdiff_t>(*fromIdx),
            first + static_cast<std::ptrdiff_t>(*fromIdx + 1)
        );
    }
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveScene::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveScene::label() const { return "playback.refactorEditor.history.moveScene"; }

DeleteScene::DeleteScene(std::string sceneId) : mSceneId(std::move(sceneId)) {}

void DeleteScene::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.scenes.size() <= 1) return;
    auto const idx = findSceneIndex(state, mSceneId);
    if (!idx) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                   = state;
    bool const deletingActive = state.scenes[*idx].id == state.activeSceneId;
    state.scenes.erase(state.scenes.begin() + static_cast<std::ptrdiff_t>(*idx));
    if (deletingActive) {
        size_t const nextIdx = std::min(*idx, state.scenes.size() - 1);
        state.activeSceneId  = state.scenes[nextIdx].id;
        ClipOps::loadWorkingFromActiveScene(state);
    }
    ClipOps::syncActiveSceneFromWorking(state);
}

void        DeleteScene::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteScene::label() const { return "playback.refactorEditor.history.deleteScene"; }

SwitchScene::SwitchScene(std::string sceneId) : mSceneId(std::move(sceneId)) {}

void SwitchScene::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const idx = findSceneIndex(state, mSceneId);
    if (!idx || state.scenes[*idx].id == state.activeSceneId) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore             = state;
    state.activeSceneId = state.scenes[*idx].id;
    ClipOps::loadWorkingFromActiveScene(state);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SwitchScene::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SwitchScene::label() const { return "playback.refactorEditor.history.switchScene"; }

SetSceneExportRange::SetSceneExportRange(int inTick, int outTick) : mInTick(inTick), mOutTick(outTick) {}

void SetSceneExportRange::execute(model::EditorStateExt& state) {
    mBefore.reset();
    int newIn  = mInTick;
    int newOut = mOutTick;
    if (newIn < 0 && newOut < 0) {
        newIn  = -1;
        newOut = -1;
    } else {
        if (state.totalTicks <= 0) return;
        if (newIn >= 0) newIn = std::clamp(newIn, 0, state.totalTicks);
        else newIn = -1;
        if (newOut >= 0) newOut = std::clamp(newOut, 0, state.totalTicks);
        else newOut = -1;
        if (newIn >= 0 && newOut >= 0 && newIn >= newOut) return;
    }

    if (state.exportInTick == newIn && state.exportOutTick == newOut) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore             = state;
    state.exportInTick  = newIn;
    state.exportOutTick = newOut;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetSceneExportRange::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetSceneExportRange::label() const { return "playback.refactorEditor.history.setSceneExportRange"; }

} // namespace playback::state::editing::command
