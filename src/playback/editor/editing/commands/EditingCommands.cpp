#include "EditingCommands.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace playback::editor::editing::command {

using model::Clip;
using model::EditorStateExt;
using model::Track;
using model::Transition;
using model::TransitionKind;

// ===== AddClipCommand =====

AddClipCommand::AddClipCommand(const std::string& trackId, const Clip& clip)
    : mTrackId(trackId), mClip(clip) {}

void AddClipCommand::execute(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    mAddedClipId = mClip.id;
    it->clips.push_back(mClip);
    std::sort(it->clips.begin(), it->clips.end(),
        [](const Clip& a, const Clip& b) { return a.trackTick < b.trackTick; });
}

void AddClipCommand::undo(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    auto clipIt = std::remove_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mAddedClipId; });
    if (clipIt != it->clips.end()) {
        it->clips.erase(clipIt, it->clips.end());
    }
}

// ===== RemoveClipCommand =====

RemoveClipCommand::RemoveClipCommand(const std::string& trackId, const std::string& clipId)
    : mTrackId(trackId), mClipId(clipId) {}

void RemoveClipCommand::execute(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    auto clipIt = std::find_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mClipId; });
    if (clipIt == it->clips.end()) return;

    mSavedClip  = *clipIt;
    mSavedIndex = std::distance(it->clips.begin(), clipIt);
    it->clips.erase(clipIt);
}

void RemoveClipCommand::undo(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    if (mSavedIndex <= it->clips.size()) {
        it->clips.insert(it->clips.begin() + mSavedIndex, mSavedClip);
    } else {
        it->clips.push_back(mSavedClip);
    }
}

// ===== SplitClipCommand =====

SplitClipCommand::SplitClipCommand(const std::string& trackId, const std::string& clipId, int atTick)
    : mTrackId(trackId), mClipId(clipId), mAtTick(atTick) {}

void SplitClipCommand::execute(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    auto clipIt = std::find_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mClipId; });
    if (clipIt == it->clips.end()) return;

    int localTick = mAtTick - clipIt->trackTick;
    int clipLen = clipIt->outTick - clipIt->inTick;
    if (localTick <= 0 || localTick >= clipLen) return;

    mOldOutTick = clipIt->outTick;

    // Right half
    mRightClip = *clipIt;
    mRightClip.id        = "";
    mRightClip.inTick    = clipIt->inTick + localTick;
    mRightClip.trackTick = clipIt->trackTick + localTick;

    // Left half: trim out
    clipIt->outTick = clipIt->inTick + localTick;

    // Generate ID for right clip
    mRightClipId = std::format("split_{}", mClipId);
    mRightClip.id = mRightClipId;

    it->clips.push_back(mRightClip);
    std::sort(it->clips.begin(), it->clips.end(),
        [](const Clip& a, const Clip& b) { return a.trackTick < b.trackTick; });
}

void SplitClipCommand::undo(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    // Remove the right clip
    auto clipIt = std::remove_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mRightClipId; });
    if (clipIt != it->clips.end()) {
        it->clips.erase(clipIt, it->clips.end());
    }

    // Restore original left clip outTick
    auto leftIt = std::find_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mClipId; });
    if (leftIt != it->clips.end()) {
        leftIt->outTick = mOldOutTick;
    }
}

std::string SplitClipCommand::label() const {
    return std::format("Split Clip at {}", mAtTick);
}

// ===== TrimClipCommand =====

TrimClipCommand::TrimClipCommand(const std::string& trackId, const std::string& clipId,
                                 int newInTick, int newOutTick)
    : mTrackId(trackId), mClipId(clipId), mNewInTick(newInTick), mNewOutTick(newOutTick) {}

void TrimClipCommand::execute(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    auto clipIt = std::find_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mClipId; });
    if (clipIt == it->clips.end()) return;

    mOldInTick  = clipIt->inTick;
    mOldOutTick = clipIt->outTick;

    if (mNewOutTick - mNewInTick > 0) {
        clipIt->inTick  = mNewInTick;
        clipIt->outTick = mNewOutTick;
    }
}

void TrimClipCommand::undo(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    auto clipIt = std::find_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mClipId; });
    if (clipIt == it->clips.end()) return;

    clipIt->inTick  = mOldInTick;
    clipIt->outTick = mOldOutTick;
}

// ===== MoveClipCommand =====

MoveClipCommand::MoveClipCommand(const std::string& trackId, const std::string& clipId, int newTrackTick)
    : mTrackId(trackId), mClipId(clipId), mNewTrackTick(newTrackTick) {}

void MoveClipCommand::execute(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    auto clipIt = std::find_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mClipId; });
    if (clipIt == it->clips.end()) return;

    mOldTrackTick = clipIt->trackTick;
    clipIt->trackTick = mNewTrackTick;

    std::sort(it->clips.begin(), it->clips.end(),
        [](const Clip& a, const Clip& b) { return a.trackTick < b.trackTick; });
}

void MoveClipCommand::undo(EditorStateExt& s) {
    auto it = std::find_if(s.videoTracks.begin(), s.videoTracks.end(),
        [&](const Track& t) { return t.id == mTrackId; });
    if (it == s.videoTracks.end()) return;

    auto clipIt = std::find_if(it->clips.begin(), it->clips.end(),
        [&](const Clip& c) { return c.id == mClipId; });
    if (clipIt == it->clips.end()) return;

    clipIt->trackTick = mOldTrackTick;
    std::sort(it->clips.begin(), it->clips.end(),
        [](const Clip& a, const Clip& b) { return a.trackTick < b.trackTick; });
}

// ===== AddTransitionCommand =====

AddTransitionCommand::AddTransitionCommand(const std::string& fromClipId, const std::string& toClipId,
                                           TransitionKind kind, int durationTicks)
    : mFromClipId(fromClipId), mToClipId(toClipId), mKind(kind), mDurationTicks(durationTicks) {}

void AddTransitionCommand::execute(EditorStateExt& s) {
    Transition t;
    t.id           = std::format("trans_{}_{}", mFromClipId, mToClipId);
    t.kind         = mKind;
    t.durationTicks = mDurationTicks;
    t.fromClipId   = mFromClipId;
    t.toClipId     = mToClipId;

    mAddedTransitionId = t.id;
    s.transitions.push_back(t);
}

void AddTransitionCommand::undo(EditorStateExt& s) {
    auto it = std::remove_if(s.transitions.begin(), s.transitions.end(),
        [&](const Transition& t) { return t.id == mAddedTransitionId; });
    if (it != s.transitions.end()) {
        s.transitions.erase(it, s.transitions.end());
    }
}

} // namespace playback::editor::editing::command
