#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace playback::editor {

enum class EditorActionType {
    TogglePause,
    Seek,
    SkipToStart,
    SkipToEnd,
    DecreaseSpeed,
    IncreaseSpeed,
    StopReplay,
    OpenReplayBrowser,
    CloseReplayBrowser,
    RefreshReplayBrowser,
    OpenReplay,
    ImportReplay,
    DeleteReplays,
    RenameReplay,
    ShowReplayInFolder,
    ClearReplayBrowserError,
};

struct EditorAction {
    EditorActionType         type{};
    int                      tick{};
    std::filesystem::path    path;
    std::string              replayId;
    std::string              name;
    std::vector<std::string> replayIds;
};

} // namespace playback::editor
