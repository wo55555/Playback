#include "AudioCommands.h"

#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace playback::state::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

std::string deriveAudioClipName(std::string const& filePath) {
    if (filePath.empty()) return "Audio";
    auto const* begin = reinterpret_cast<char8_t const*>(filePath.data());
    auto const  stem  = std::filesystem::path(std::u8string{begin, begin + filePath.size()}).stem().u8string();
    if (stem.empty()) return "Audio";
    return {reinterpret_cast<char const*>(stem.data()), stem.size()};
}

model::AudioTrack* resolveAudioTrack(model::EditorStateExt& state, std::string_view trackId, std::string_view clipId) {
    if (!trackId.empty()) return ClipOps::findAudioTrack(state, trackId);
    for (auto& track : state.audioTracks) {
        if (ClipOps::findAudioClip(track, clipId) != nullptr) return &track;
    }
    return nullptr;
}

void sortAudioClips(std::vector<model::AudioClip>& clips) {
    std::ranges::stable_sort(clips, {}, &model::AudioClip::startTick);
}

int clampMoveStartTick(int tick, int totalTicks) noexcept {
    if (totalTicks > 0) return std::clamp(tick, 0, totalTicks);
    return std::max(0, tick);
}

} // namespace

AddAudioTrack::AddAudioTrack(std::string name) : mName(std::move(name)) {}

void AddAudioTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    model::AudioTrack track;
    track.id   = ClipOps::nextAudioTrackId(state);
    track.name = mName.empty() ? ClipOps::defaultAudioTrackName(state) : mName;
    state.audioTracks.push_back(std::move(track));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddAudioTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddAudioTrack::label() const { return "playback.refactorEditor.history.addAudioTrack"; }

RemoveAudioTrack::RemoveAudioTrack(std::string trackId) : mTrackId(std::move(trackId)) {}

void RemoveAudioTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const it = std::ranges::find(state.audioTracks, mTrackId, &model::AudioTrack::id);
    if (it == state.audioTracks.end() || it->locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore            = state;
    auto const eraseIt = std::ranges::find(state.audioTracks, mTrackId, &model::AudioTrack::id);
    if (eraseIt != state.audioTracks.end()) state.audioTracks.erase(eraseIt);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveAudioTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveAudioTrack::label() const { return "playback.refactorEditor.history.removeAudioTrack"; }

RenameAudioTrack::RenameAudioTrack(std::string trackId, std::string name)
: mTrackId(std::move(trackId)),
  mName(std::move(name)) {}

void RenameAudioTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto* track = ClipOps::findAudioTrack(state, mTrackId);
    if (!track || track->locked || mName.empty() || track->name == mName) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore     = state;
    track->name = mName;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RenameAudioTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RenameAudioTrack::label() const { return "playback.refactorEditor.history.renameAudioTrack"; }

SetAudioTrackState::SetAudioTrackState(std::string trackId, Property property, bool value)
: mTrackId(std::move(trackId)),
  mProperty(property),
  mBoolValue(value) {}

SetAudioTrackState::SetAudioTrackState(std::string trackId, float volume)
: mTrackId(std::move(trackId)),
  mProperty(Property::Volume),
  mVolumeValue(volume) {}

void SetAudioTrackState::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto* track = ClipOps::findAudioTrack(state, mTrackId);
    if (!track) return;
    if (mProperty != Property::Locked && track->locked) return;

    if (mProperty == Property::Volume) {
        float const clamped = model::clampAudioVolume(mVolumeValue);
        if (std::abs(track->volume - clamped) < 1e-6f) return;

        ClipOps::syncActiveSceneFromWorking(state);
        mBefore       = state;
        track->volume = clamped;
        ClipOps::syncActiveSceneFromWorking(state);
        return;
    }

    bool* target = nullptr;
    switch (mProperty) {
    case Property::Enabled:
        target = &track->enabled;
        break;
    case Property::Muted:
        target = &track->muted;
        break;
    case Property::Locked:
        target = &track->locked;
        break;
    case Property::Volume:
        return;
    }
    if (*target == mBoolValue) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    *target = mBoolValue;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetAudioTrackState::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetAudioTrackState::label() const {
    switch (mProperty) {
    case Property::Enabled:
        return "playback.refactorEditor.history.setAudioTrackEnabled";
    case Property::Muted:
        return "playback.refactorEditor.history.setAudioTrackMuted";
    case Property::Locked:
        return "playback.refactorEditor.history.setAudioTrackLocked";
    case Property::Volume:
        return "playback.refactorEditor.history.setAudioTrackVolume";
    }
    return "playback.refactorEditor.history.setAudioTrackEnabled";
}

AddAudioClip::AddAudioClip(std::string trackId, std::string filePath, int startTick, std::string name)
: mTrackId(std::move(trackId)),
  mFilePath(std::move(filePath)),
  mStartTick(startTick),
  mName(std::move(name)) {}

void AddAudioClip::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto* track = ClipOps::findAudioTrack(state, mTrackId);
    if (!track || track->locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    model::AudioClip clip;
    clip.id        = ClipOps::nextAudioClipId(state);
    clip.name      = mName.empty() ? deriveAudioClipName(mFilePath) : mName;
    clip.filePath  = mFilePath;
    clip.startTick = std::max(0, mStartTick);
    track->clips.push_back(std::move(clip));
    sortAudioClips(track->clips);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddAudioClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddAudioClip::label() const { return "playback.refactorEditor.history.addAudioClip"; }

RemoveAudioClip::RemoveAudioClip(std::string trackId, std::string clipId)
: mTrackId(std::move(trackId)),
  mClipId(std::move(clipId)) {}

void RemoveAudioClip::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto* track = resolveAudioTrack(state, mTrackId, mClipId);
    if (!track || track->locked) return;

    auto const it = std::ranges::find(track->clips, mClipId, &model::AudioClip::id);
    if (it == track->clips.end()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore            = state;
    auto const eraseIt = std::ranges::find(track->clips, mClipId, &model::AudioClip::id);
    if (eraseIt != track->clips.end()) track->clips.erase(eraseIt);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveAudioClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveAudioClip::label() const { return "playback.refactorEditor.history.removeAudioClip"; }

MoveAudioClip::MoveAudioClip(std::string trackId, std::string clipId, int startTick)
: mTrackId(std::move(trackId)),
  mClipId(std::move(clipId)),
  mStartTick(startTick) {}

void MoveAudioClip::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto* track = resolveAudioTrack(state, mTrackId, mClipId);
    if (!track || track->locked) return;

    auto* clip = ClipOps::findAudioClip(*track, mClipId);
    if (!clip) return;

    int const clampedTick = clampMoveStartTick(mStartTick, state.totalTicks);
    if (clip->startTick == clampedTick) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore         = state;
    clip->startTick = clampedTick;
    sortAudioClips(track->clips);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveAudioClip::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveAudioClip::label() const { return "playback.refactorEditor.history.moveAudioClip"; }

SetAudioClipProps::SetAudioClipProps(
    std::string trackId,
    std::string clipId,
    std::string filePath,
    double      trimStartSeconds,
    double      durationSeconds,
    float       volume,
    bool        muted
)
: mTrackId(std::move(trackId)),
  mClipId(std::move(clipId)),
  mFilePath(std::move(filePath)),
  mTrimStartSeconds(trimStartSeconds),
  mDurationSeconds(durationSeconds),
  mVolume(volume),
  mMuted(muted) {}

void SetAudioClipProps::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto* track = resolveAudioTrack(state, mTrackId, mClipId);
    if (!track || track->locked) return;

    auto* clip = ClipOps::findAudioClip(*track, mClipId);
    if (!clip) return;

    std::string const newFilePath = mFilePath.empty() ? clip->filePath : mFilePath;
    std::string       newName     = clip->name;
    if (!mFilePath.empty() && mFilePath != clip->filePath) {
        newName = deriveAudioClipName(mFilePath);
    }
    double const newTrimStart = model::clampAudioSeconds(mTrimStartSeconds);
    double const newDuration  = model::clampAudioSeconds(mDurationSeconds);
    float const  newVolume    = model::clampAudioVolume(mVolume);
    bool const   newMuted     = mMuted;

    if (newFilePath == clip->filePath && newName == clip->name && std::abs(newTrimStart - clip->trimStartSeconds) < 1e-9
        && std::abs(newDuration - clip->durationSeconds) < 1e-9 && std::abs(newVolume - clip->volume) < 1e-6f
        && newMuted == clip->muted) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                = state;
    clip->filePath         = newFilePath;
    clip->name             = std::move(newName);
    clip->trimStartSeconds = newTrimStart;
    clip->durationSeconds  = newDuration;
    clip->volume           = newVolume;
    clip->muted            = newMuted;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetAudioClipProps::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetAudioClipProps::label() const { return "playback.refactorEditor.history.setAudioClip"; }

} // namespace playback::state::editing::command
