#pragma once

#include "playback/editor/ui/PanelContext.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace playback::editor::ui {

class EditorMenuBar {
public:
    void draw(PanelContext const& ctx);
    void openExportDialog(int totalTicks, bool ffmpegAvailable, int exportInTick, int exportOutTick);
    void openExportSingleClip(
        std::string const& clipId,
        int                totalTicks,
        bool               ffmpegAvailable,
        int                exportInTick,
        int                exportOutTick
    );
    void openExportScene(
        std::string const& sceneId,
        std::string const& activeSceneId,
        std::size_t        sceneCount,
        int                totalTicks,
        bool               ffmpegAvailable,
        int                exportInTick,
        int                exportOutTick
    );

    [[nodiscard]] int exportFps() const { return mFps; }

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
    int  mExportExtraRenders{0};
    int  mExportEncodeSpeed{0};
    int  mExportCrf{18};
    bool mExportHideHud{false};
    bool mExportAudio{true};
    int  mExportAudioChannels{1};
    int  mExportAudioCodec{0};
    int  mExportAudioBitrate{1};
    // Page of the export dialog being shown.
    int mExportTab{0};
    // Height shared by every page: the tallest one measured so far.
    float mExportBodyHeight{0.0f};
    bool  mExportBodyScrollable{false};
    // Font size the page height was measured at.
    float                    mExportLayoutKey{0.0f};
    int                      mExportLayoutRangeMode{-1};
    int                      mExportLayoutSceneScope{-1};
    bool                     mExportLayoutMultiScene{false};
    int                      mExportStartTick{-1};
    int                      mExportEndTick{-1};
    int                      mExportRangeMode{0};
    int                      mExportSceneScope{0}; // 0: active scene, 1: selected scenes
    bool                     mExportSplitClips{false};
    std::string              mExportSingleClipId;
    std::vector<std::string> mExportSelectedSceneIds;
    std::array<char, 128>    mExportName{"replay-export"};
    std::array<char, 260>    mExportDirectory{"mods/playback/exports"};
};

} // namespace playback::editor::ui
