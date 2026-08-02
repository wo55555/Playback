#include "ClipEditor.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>
#include <stdexcept>

namespace playback::editor::editing {

using model::Clip;
using model::Track;

namespace {

std::string genUuidImpl() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    return std::format("{:016x}", dist(rng));
}

Track* findTrackById(std::vector<Track>& tracks, const std::string& id) {
    auto it = std::find_if(tracks.begin(), tracks.end(),
        [&](const Track& t) { return t.id == id; });
    return (it != tracks.end()) ? &*it : nullptr;
}

const Track* findTrackById(const std::vector<Track>& tracks, const std::string& id) {
    auto it = std::find_if(tracks.begin(), tracks.end(),
        [&](const Track& t) { return t.id == id; });
    return (it != tracks.end()) ? &*it : nullptr;
}

std::vector<Clip>::iterator findClipIter(Track& track, const std::string& clipId) {
    return std::find_if(track.clips.begin(), track.clips.end(),
        [&](const Clip& c) { return c.id == clipId; });
}

} // namespace

std::string ClipEditor::genUuid() {
    return genUuidImpl();
}

std::vector<Clip> ClipEditor::cut(std::vector<Track>& tracks, const std::string& trackId,
                                   const std::string& clipId) {
    auto* track = findTrackById(tracks, trackId);
    if (!track || track->locked) return {};

    auto it = findClipIter(*track, clipId);
    if (it == track->clips.end() || it->locked) return {};

    std::vector<Clip> result;
    result.push_back(*it);
    track->clips.erase(it);
    return result;
}

std::vector<Clip> ClipEditor::copyClip(const std::vector<Track>& tracks, const std::string& trackId,
                                        const std::string& clipId) {
    const auto* track = findTrackById(tracks, trackId);
    if (!track) return {};

    auto it = std::find_if(track->clips.begin(), track->clips.end(),
        [&](const Clip& c) { return c.id == clipId; });
    if (it == track->clips.end()) return {};

    return {*it};
}

std::optional<std::string> ClipEditor::paste(std::vector<Track>& tracks, const std::string& trackId,
                                              int atTick, const std::vector<Clip>& clipboard) {
    auto* track = findTrackById(tracks, trackId);
    if (!track || clipboard.empty() || track->locked) return {};

    Clip c = clipboard.front();
    c.id       = genUuidImpl();
    c.trackTick = atTick;

    track->clips.push_back(c);
    std::sort(track->clips.begin(), track->clips.end(),
        [](const Clip& a, const Clip& b) { return a.trackTick < b.trackTick; });

    return c.id;
}

bool ClipEditor::split(std::vector<Track>& tracks, const std::string& trackId,
                       const std::string& clipId, int atTick) {
    auto* track = findTrackById(tracks, trackId);
    if (!track || track->locked) return false;

    auto it = findClipIter(*track, clipId);
    if (it == track->clips.end() || it->locked) return false;

    int localTick = atTick - it->trackTick;
    int clipLen = it->outTick - it->inTick;
    if (localTick <= 0 || localTick >= clipLen) return false;

    // Right half
    Clip right = *it;
    right.id        = genUuidImpl();
    right.inTick    = it->inTick + localTick;
    right.trackTick = it->trackTick + localTick;

    // Left half: trim out
    it->outTick = it->inTick + localTick;

    track->clips.push_back(right);
    std::sort(track->clips.begin(), track->clips.end(),
        [](const Clip& a, const Clip& b) { return a.trackTick < b.trackTick; });

    return true;
}

bool ClipEditor::trim(std::vector<Track>& tracks, const std::string& trackId,
                      const std::string& clipId, int newInTick, int newOutTick) {
    auto* track = findTrackById(tracks, trackId);
    if (!track || track->locked) return false;

    auto it = findClipIter(*track, clipId);
    if (it == track->clips.end() || it->locked) return false;

    if (newOutTick - newInTick <= 0) return false;

    int oldIn  = it->inTick;
    int oldOut = it->outTick;

    it->inTick  = newInTick;
    it->outTick = newOutTick;

    if (it->outTick - it->inTick <= 0) {
        it->inTick  = oldIn;
        it->outTick = oldOut;
        return false;
    }

    return true;
}

bool ClipEditor::rippleDelete(std::vector<Track>& tracks, const std::string& trackId,
                              const std::string& clipId) {
    auto* track = findTrackById(tracks, trackId);
    if (!track || track->locked) return false;

    auto it = findClipIter(*track, clipId);
    if (it == track->clips.end()) return false;

    int removeLen = it->outTick - it->inTick;
    int clipTrackTick = it->trackTick;

    track->clips.erase(it);

    // Shift subsequent clips
    for (auto& c : track->clips) {
        if (c.trackTick > clipTrackTick) {
            c.trackTick -= removeLen;
        }
    }

    return true;
}

bool ClipEditor::move(std::vector<Track>& tracks, const std::string& trackId,
                      const std::string& clipId, int newTrackTick) {
    auto* track = findTrackById(tracks, trackId);
    if (!track || track->locked) return false;

    auto it = findClipIter(*track, clipId);
    if (it == track->clips.end() || it->locked) return false;

    it->trackTick = newTrackTick;
    std::sort(track->clips.begin(), track->clips.end(),
        [](const Clip& a, const Clip& b) { return a.trackTick < b.trackTick; });

    return true;
}

bool ClipEditor::setSpeed(std::vector<Track>& tracks, const std::string& trackId,
                          const std::string& clipId, float speed) {
    auto* track = findTrackById(tracks, trackId);
    if (!track || track->locked) return false;

    auto it = findClipIter(*track, clipId);
    if (it == track->clips.end() || it->locked) return false;

    it->speed = std::max(0.1f, std::min(10.0f, speed));
    return true;
}

} // namespace playback::editor::editing
