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

// The same search and list drawn inline at the available width, for the inspector. Returns true on a pick.
// `focusSearch` puts the caret in the search box this frame (the segment menu jumping here).
bool drawPlayerList(
    char const*                             listId,
    PlayerPickerMode                        mode,
    std::span<state::ReplayPlayerView const> players,
    std::string_view                        current,
    bool                                    focusSearch,
    PlayerPickerResult&                     result
);

} // namespace playback::editor::ui
