#pragma once

#include "playback/editor/ui/PanelContext.h"

#include <array>
#include <string>

namespace playback::editor::ui {

class EditorMenuBar {
public:
    void               draw(PanelContext const& ctx);
    void               openExportDialog(int totalTicks, bool ffmpegAvailable);
    [[nodiscard]] bool isAnyMenuOpen() const;

    // The timeline draws these as in/out markers and can drag them; -1 means "whole replay".
    [[nodiscard]] int exportStartTick() const { return mExportStartTick; }
    [[nodiscard]] int exportEndTick() const { return mExportEndTick; }
    void              setExportStartTick(int tick) { mExportStartTick = tick; }
    void              setExportEndTick(int tick) { mExportEndTick = tick; }
    // Marks in/out at the playhead, clearing the range back to unset once it covers everything.
    void markExportPoint(bool isIn, int tick, int totalTicks);

private:
    void drawMenus(PanelContext const& ctx);
    void drawShortcutDialog();
    void drawExportDialog(PanelContext const& ctx);

    bool mExportDialogOpen{false};
    bool mExportSettingsInitialized{false};
    bool mShortcutDialogOpen{false};
    int  mExportFormat{0};
    int  mFpsPreset{1};
    int  mFps{60};
    int  mExportWidth{1920};
    int  mExportHeight{1080};
    int  mExportSsaa{0};
    int  mExportWarmupFrames{60};
    int  mExportConvergenceFrames{0};
    bool mExportAccelerateClient{false};
    int  mExportEncodeSpeed{0};
    int  mExportCrf{18};
    // Page of the export dialog being shown.
    int mExportTab{0};
    // Height shared by every page: the tallest one measured so far.
    float mExportBodyHeight{0.0f};
    bool  mExportBodyScrollable{false};
    // Font size the page height was measured at.
    float                 mExportLayoutKey{0.0f};
    int                   mExportStartTick{-1};
    int                   mExportEndTick{-1};
    std::array<char, 128> mExportName{"replay-export"};
    std::array<char, 260> mExportDirectory{"mods/playback/exports"};
};

} // namespace playback::editor::ui
