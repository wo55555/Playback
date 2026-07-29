#pragma once

#include "playback/refactor/editor/models/Track.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::refactor::video_editing {

using namespace playback::refactor::editor;

class ClipEditor {
public:
    // Cut: copy to clipboard + delete
    static std::vector<Clip> cut(std::vector<Track>& tracks, const std::string& trackId,
                                  const std::string& clipId);

    // Copy: copy to clipboard (doesn't delete)
    static std::vector<Clip> copyClip(const std::vector<Track>& tracks, const std::string& trackId,
                                       const std::string& clipId);

    // Paste: insert from clipboard at given tick
    static std::optional<std::string> paste(std::vector<Track>& tracks, const std::string& trackId,
                                             int atTick, const std::vector<Clip>& clipboard);

    // Split: split clip at timeline tick
    static bool split(std::vector<Track>& tracks, const std::string& trackId,
                      const std::string& clipId, int atTick);

    // Trim: adjust in/out ticks
    static bool trim(std::vector<Track>& tracks, const std::string& trackId,
                     const std::string& clipId, int newInTick, int newOutTick);

    // Ripple delete: remove clip + shift subsequent clips
    static bool rippleDelete(std::vector<Track>& tracks, const std::string& trackId,
                             const std::string& clipId);

    // Move: change trackTick
    static bool move(std::vector<Track>& tracks, const std::string& trackId,
                     const std::string& clipId, int newTrackTick);

    // Set speed
    static bool setSpeed(std::vector<Track>& tracks, const std::string& trackId,
                         const std::string& clipId, float speed);

private:
    static std::string genUuid();
};

} // namespace playback::refactor::video_editing