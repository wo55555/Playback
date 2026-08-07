#pragma once

#include <array>

namespace playback::editor::ui {

class EditorMenuBar {
public:
    void               draw();
    [[nodiscard]] bool isAnyMenuOpen() const;

private:
    bool                  mExportDialogOpen{false};
    bool                  mShortcutDialogOpen{false};
    int                   mExportFormat{0};
    int                   mFpsPreset{1};
    int                   mFps{60};
    int                   mExportStartTick{};
    int                   mExportEndTick{};
    std::array<char, 128> mExportName{"replay-export"};
    std::array<char, 260> mExportDirectory{"mods/playback/exports"};
};

} // namespace playback::editor::ui
