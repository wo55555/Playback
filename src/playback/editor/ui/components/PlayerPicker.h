#pragma once

#include "playback/state/EditorState.h"

#include <span>
#include <string>
#include <string_view>

namespace playback::editor::ui {

enum class PlayerPickerMode { Follow, ExportSubject, SegmentSubject };

struct PlayerPickerResult {
    bool        picked{};
    bool        stop{};
    std::string uuid;
};

// Empty uuid is the recorder. Shared by the viewport chip, the segment menu, and the export dialog.
[[nodiscard]] std::string playerSubjectLabel(std::span<state::ReplayPlayerView const> players, std::string_view uuid);

// Draws an anchored popup opened by the caller. Fills `result` and returns true only when the user
// picked a row or stopped following; opening, searching, and scrolling submit nothing.
bool drawPlayerPicker(
    char const*                             popupId,
    PlayerPickerMode                        mode,
    std::span<state::ReplayPlayerView const> players,
    std::string_view                        current,
    bool                                    following,
    PlayerPickerResult&                     result
);

} // namespace playback::editor::ui
