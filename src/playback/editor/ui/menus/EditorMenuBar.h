#pragma once

#include <array>

namespace playback::editor::ui {

class EditorMenuBar {
public:
    void               draw();
    void               openExportDialog(int totalTicks, bool ffmpegAvailable);
    [[nodiscard]] bool isAnyMenuOpen() const;

private:
    bool                  mExportDialogOpen{false};
    bool                  mExportSettingsInitialized{false};
    bool                  mShortcutDialogOpen{false};
    int                   mExportFormat{0};
    int                   mFpsPreset{1};
    int                   mFps{60};
    int                   mExportWidth{1920};
    int                   mExportHeight{1080};
    int                   mExportSsaa{0};
    int                   mExportWarmupFrames{60};
    int                   mExportStartTick{};
    int                   mExportEndTick{};
    std::array<char, 128> mExportName{"replay-export"};
    std::array<char, 260> mExportDirectory{"mods/playback/exports"};
};

} // namespace playback::editor::ui
