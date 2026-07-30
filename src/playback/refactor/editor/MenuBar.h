#pragma once

#include <array>

namespace playback::refactor::editor {

class MenuBar {
public:
    void draw();
    [[nodiscard]] bool isAnyMenuOpen() const;

private:
    bool mExportDialogOpen{false};
    int mAspectPreset{};
    int mResolutionPreset{};
    int mFpsPreset{1};
    int mBitratePreset{1};
    int mFormatPreset{};
    int mWidth{1920};
    int mHeight{1080};
    int mFps{60};
    int mBitrateMbps{20};
    std::array<char, 128> mExportName{"replay-export"};
    std::array<char, 260> mExportDirectory{"mods/playback/exports"};
};

} // namespace playback::refactor::editor
