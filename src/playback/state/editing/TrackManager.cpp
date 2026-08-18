#include "TrackManager.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>
#include <stdexcept>

namespace playback::state::editing {

using model::Clip;
using model::Color4;
using model::EditorStateExt;
using model::Track;
using model::TrackKind;
using model::Transition;
using model::TransitionKind;

namespace {

std::string genUuid() {
    static std::mt19937                            rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    return std::format("{:016x}", dist(rng));
}

} // namespace

TrackManager& TrackManager::getInstance() {
    static TrackManager instance;
    return instance;
}

void TrackManager::setEditorState(const EditorStateExt& state) {
    std::scoped_lock lk(mMtx);
    mState = state;
}

EditorStateExt TrackManager::snapshot() const {
    std::scoped_lock lk(mMtx);
    return mState;
}

// ===== Track operations =====

std::string TrackManager::addTrack(TrackKind kind, const std::string& name) {
    std::scoped_lock lk(mMtx);

    Track track;
    track.id     = genUuid();
    track.name   = name;
    track.kind   = kind;
    track.height = (kind == TrackKind::Marker) ? 20 : 48;

    mState.videoTracks.push_back(std::move(track));
    return mState.videoTracks.back().id;
}

void TrackManager::removeTrack(const std::string& id) {
    std::scoped_lock lk(mMtx);
    auto             it = std::remove_if(mState.videoTracks.begin(), mState.videoTracks.end(), [&](const Track& t) {
        return t.id == id;
    });
    if (it != mState.videoTracks.end()) {
        mState.videoTracks.erase(it, mState.videoTracks.end());
    }
}

void TrackManager::reorderTrack(const std::string& id, int newIndex) {
    std::scoped_lock lk(mMtx);
    auto             it =
        std::find_if(mState.videoTracks.begin(), mState.videoTracks.end(), [&](const Track& t) { return t.id == id; });
    if (it == mState.videoTracks.end()) return;

    Track track = std::move(*it);
    mState.videoTracks.erase(it);

    newIndex = std::clamp(newIndex, 0, static_cast<int>(mState.videoTracks.size()));
    mState.videoTracks.insert(mState.videoTracks.begin() + newIndex, std::move(track));
}

// ===== Clip operations =====

std::string TrackManager::addClip(const std::string& trackId, const Clip& clip) {
    std::scoped_lock lk(mMtx);
    auto&            track = findTrack(trackId);
    if (track.locked) return {};

    Clip c  = clip;
    c.id    = genUuid();
    c.color = pickColorFor(c.replayFile);
    track.clips.push_back(c);
    sortClipsByTick(track);
    return c.id;
}

void TrackManager::removeClip(const std::string& trackId, const std::string& clipId) {
    std::scoped_lock lk(mMtx);
    auto&            track = findTrack(trackId);
    if (track.locked) return;

    auto it = findClipIter(track, clipId);
    if (it != track.clips.end()) {
        track.clips.erase(it);
    }
}

void TrackManager::moveClip(const std::string& trackId, const std::string& clipId, int newTrackTick) {
    std::scoped_lock lk(mMtx);
    auto&            clip = findClip(trackId, clipId);
    if (clip.locked) return;

    clip.trackTick = newTrackTick;
    auto& track    = findTrack(trackId);
    sortClipsByTick(track);
}

void TrackManager::trimClip(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick) {
    std::scoped_lock lk(mMtx);
    auto&            clip = findClip(trackId, clipId);
    if (clip.locked) return;

    int len = newOutTick - newInTick;
    if (len <= 0) return;

    int oldIn  = clip.inTick;
    int oldOut = clip.outTick;

    clip.inTick  = newInTick;
    clip.outTick = newOutTick;

    if (clip.outTick - clip.inTick <= 0) {
        // Rollback
        clip.inTick  = oldIn;
        clip.outTick = oldOut;
    }
}

void TrackManager::splitClip(const std::string& trackId, const std::string& clipId, int atTick) {
    std::scoped_lock lk(mMtx);
    auto&            track = findTrack(trackId);
    if (track.locked) return;

    auto it = findClipIter(track, clipId);
    if (it == track.clips.end() || it->locked) return;

    int localTick = atTick - it->trackTick; // Convert to clip-internal tick
    int clipLen   = it->outTick - it->inTick;
    if (localTick <= 0 || localTick >= clipLen) return;

    // Right half
    Clip right      = *it;
    right.id        = genUuid();
    right.inTick    = it->inTick + localTick;
    right.trackTick = it->trackTick + localTick;

    // Left half: trim out
    it->outTick = it->inTick + localTick;

    track.clips.push_back(right);
    sortClipsByTick(track);
}

void TrackManager::rippleDelete(const std::string& trackId, const std::string& clipId) {
    std::scoped_lock lk(mMtx);
    auto&            track = findTrack(trackId);
    if (track.locked) return;

    auto& clip      = findClip(trackId, clipId);
    int   removeLen = clip.outTick - clip.inTick;

    auto it = findClipIter(track, clipId);
    if (it == track.clips.end()) return;
    track.clips.erase(it);

    // Shift all subsequent clips left by removeLen
    for (auto& c : track.clips) {
        if (c.trackTick > clip.trackTick) {
            c.trackTick -= removeLen;
        }
    }
}

// ===== Transition operations =====

std::string TrackManager::addTransition(
    const std::string& fromClipId,
    const std::string& toClipId,
    TransitionKind     kind,
    int                durationTicks
) {
    std::scoped_lock lk(mMtx);

    Transition t;
    t.id            = genUuid();
    t.kind          = kind;
    t.durationTicks = durationTicks;
    t.fromClipId    = fromClipId;
    t.toClipId      = toClipId;

    mState.transitions.push_back(t);
    return t.id;
}

void TrackManager::removeTransition(const std::string& transitionId) {
    std::scoped_lock lk(mMtx);
    auto it = std::remove_if(mState.transitions.begin(), mState.transitions.end(), [&](const Transition& t) {
        return t.id == transitionId;
    });
    if (it != mState.transitions.end()) {
        mState.transitions.erase(it, mState.transitions.end());
    }
}

// ===== Query =====

std::vector<Clip*> TrackManager::getActiveClipsAt(int timelineTick) {
    std::scoped_lock   lk(mMtx);
    std::vector<Clip*> result;
    for (auto& t : mState.videoTracks) {
        if (!t.visible || t.kind != TrackKind::Video) continue;
        for (auto& c : t.clips) {
            int clipEnd = c.trackTick + (c.outTick - c.inTick);
            if (timelineTick >= c.trackTick && timelineTick < clipEnd) {
                result.push_back(&c);
            }
        }
    }
    return result;
}

const Transition*
TrackManager::findTransitionBetween(const std::string& fromClipId, const std::string& toClipId) const {
    std::scoped_lock lk(mMtx);
    for (const auto& t : mState.transitions) {
        if (t.fromClipId == fromClipId && t.toClipId == toClipId) {
            return &t;
        }
    }
    return nullptr;
}

// ===== Private helpers =====

Track& TrackManager::findTrack(const std::string& id) {
    auto it =
        std::find_if(mState.videoTracks.begin(), mState.videoTracks.end(), [&](const Track& t) { return t.id == id; });
    if (it == mState.videoTracks.end()) {
        throw std::runtime_error("Track not found: " + id);
    }
    return *it;
}

const Track& TrackManager::findTrack(const std::string& id) const {
    auto it =
        std::find_if(mState.videoTracks.begin(), mState.videoTracks.end(), [&](const Track& t) { return t.id == id; });
    if (it == mState.videoTracks.end()) {
        throw std::runtime_error("Track not found: " + id);
    }
    return *it;
}

std::vector<Clip>::iterator TrackManager::findClipIter(Track& track, const std::string& clipId) {
    return std::find_if(track.clips.begin(), track.clips.end(), [&](const Clip& c) { return c.id == clipId; });
}

Clip& TrackManager::findClip(const std::string& trackId, const std::string& clipId) {
    auto& track = findTrack(trackId);
    auto  it    = findClipIter(track, clipId);
    if (it == track.clips.end()) {
        throw std::runtime_error("Clip not found: " + clipId);
    }
    return *it;
}

const Clip& TrackManager::findClip(const std::string& trackId, const std::string& clipId) const {
    const auto& track = findTrack(trackId);
    auto it = std::find_if(track.clips.begin(), track.clips.end(), [&](const Clip& c) { return c.id == clipId; });
    if (it == track.clips.end()) {
        throw std::runtime_error("Clip not found: " + clipId);
    }
    return *it;
}

void TrackManager::sortClipsByTick(Track& track) {
    std::sort(track.clips.begin(), track.clips.end(), [](const Clip& a, const Clip& b) {
        return a.trackTick < b.trackTick;
    });
}

Color4 TrackManager::pickColorFor(const std::string& replayFile) {
    // Simple hash-based color assignment
    auto const h = static_cast<uint32_t>(std::hash<std::string>{}(replayFile));
    float      r = ((h >> 0) & 0xFF) / 255.0f * 0.6f + 0.2f;
    float      g = ((h >> 8) & 0xFF) / 255.0f * 0.6f + 0.2f;
    float      b = ((h >> 16) & 0xFF) / 255.0f * 0.6f + 0.2f;
    return {r, g, b, 1.0f};
}

} // namespace playback::state::editing
