#include "EditorMenuBar.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/SettingsPage.h"
#include "playback/editor/ui/iconfont.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void EditorMenuBar::draw() {
    auto&       editor       = ReplayEditor::getInstance();
    auto const& capabilities = editor.state().capabilities;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("playback.refactorEditor.menu.file"_tr().c_str())) {
            ImGui::MenuItem("playback.refactorEditor.menu.openReplay"_tr().c_str(), "Ctrl+O", false, false);
            ImGui::MenuItem("playback.refactorEditor.menu.saveProject"_tr().c_str(), "Ctrl+S", false, false);
            ImGui::MenuItem("playback.refactorEditor.menu.recent"_tr().c_str(), nullptr, false, false);
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.export"_tr().c_str(),
                    nullptr,
                    false,
                    capabilities.videoExport
                )) {
                mExportDialogOpen = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.menu.exit"_tr().c_str(), "Esc (hold)")) {
                editor.submitAction({playback::editor::EditorActionType::StopReplay});
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("playback.refactorEditor.menu.edit"_tr().c_str())) {
            ImGui::MenuItem("playback.refactorEditor.menu.undo"_tr().c_str(), "Ctrl+Z", false, false);
            ImGui::MenuItem("playback.refactorEditor.menu.redo"_tr().c_str(), "Ctrl+Y", false, false);
            ImGui::Separator();
            ImGui::MenuItem("playback.refactorEditor.menu.delete"_tr().c_str(), "Del", false, false);
            ImGui::MenuItem("playback.refactorEditor.menu.selectAll"_tr().c_str(), "Ctrl+A", false, false);
            ImGui::EndMenu();
        }

        ImGui::MenuItem("playback.refactorEditor.menu.camera"_tr().c_str(), nullptr, false, capabilities.cameraEditing);

        ImGui::MenuItem("playback.refactorEditor.menu.markers"_tr().c_str(), nullptr, false, capabilities.videoEditing);

        if (ImGui::BeginMenu("playback.refactorEditor.menu.window"_tr().c_str())) {
            ImGui::MenuItem("playback.refactorEditor.menu.hintBar"_tr().c_str(), "F1", false, false);
            ImGui::Separator();
            ImGui::MenuItem("playback.refactorEditor.menu.curveEditor"_tr().c_str(), nullptr, false, false);
            ImGui::Separator();
            if (ImGui::MenuItem("playback.settings.menu.open"_tr().c_str())) openSettingsPage(SettingsSection::Editor);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("playback.refactorEditor.menu.help"_tr().c_str())) {
            if (ImGui::MenuItem("playback.refactorEditor.menu.shortcuts"_tr().c_str())) mShortcutDialogOpen = true;
            ImGui::MenuItem("playback.refactorEditor.menu.documentation"_tr().c_str(), nullptr, false, false);
            ImGui::Separator();
            ImGui::MenuItem("playback.refactorEditor.menu.about"_tr().c_str(), nullptr, false, false);
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    if (mShortcutDialogOpen) ImGui::OpenPopup("##KeyboardShortcuts");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "##KeyboardShortcuts",
            &mShortcutDialogOpen,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
        )) {
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.title"_tr().c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.playback"_tr().c_str());
        ImGui::Separator();
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.space"_tr().c_str());
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.homeEnd"_tr().c_str());
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.arrows"_tr().c_str());
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.zoom"_tr().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.editing"_tr().c_str());
        ImGui::Separator();
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.undoRedo"_tr().c_str());
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.keyframe"_tr().c_str());
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.marker"_tr().c_str());
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.delete"_tr().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.application"_tr().c_str());
        ImGui::Separator();
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.save"_tr().c_str());
        ImGui::BulletText("%s", "playback.refactorEditor.shortcuts.exit"_tr().c_str());
        ImGui::Spacing();
        if (ImGui::Button("playback.refactorEditor.shortcuts.close"_tr().c_str(), {110.0f, 32.0f})) {
            mShortcutDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (mExportDialogOpen) ImGui::OpenPopup("##ExportVideo");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(660.0f, 0.0f),
        ImVec2(660.0f, ImGui::GetMainViewport()->WorkSize.y - 24.0f)
    );
    if (ImGui::BeginPopupModal(
            "##ExportVideo",
            &mExportDialogOpen,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
        )) {
        std::string const custom          = "playback.refactorEditor.export.custom"_tr();
        std::string const widescreen      = "16:9  " + "playback.refactorEditor.export.widescreen"_tr();
        std::string const vertical        = "9:16  " + "playback.refactorEditor.export.vertical"_tr();
        std::string const square          = "1:1  " + "playback.refactorEditor.export.square"_tr();
        const char*       aspectOptions[] = {widescreen.c_str(), vertical.c_str(), square.c_str(), custom.c_str()};
        const char*       resolutionOptions[] =
            {"1920 x 1080  Full HD", "1280 x 720  HD", "2560 x 1440  QHD", "3840 x 2160  4K", custom.c_str()};
        const char*       fpsOptions[]     = {"30 FPS", "60 FPS", "120 FPS", custom.c_str()};
        std::string const compact          = "10 Mbps  " + "playback.refactorEditor.export.compact"_tr();
        std::string const balanced         = "20 Mbps  " + "playback.refactorEditor.export.balanced"_tr();
        std::string const highQuality      = "40 Mbps  " + "playback.refactorEditor.export.highQuality"_tr();
        const char*       bitrateOptions[] = {compact.c_str(), balanced.c_str(), highQuality.c_str(), custom.c_str()};
        constexpr const char* formatOptions[]   = {"MP4", "MKV", "WebM", "MOV", "AVI"};
        constexpr const char* codecOptions[][5] = {
            {"H.264", "H.265 / HEVC", nullptr},
            {"H.264", "H.265 / HEVC", "VP9", "AV1", nullptr},
            {"VP9", "AV1", nullptr},
            {"H.264", "H.265 / HEVC", "ProRes", nullptr},
            {"H.264", "MJPEG", nullptr}
        };
        constexpr int codecCounts[]   = {2, 4, 2, 3, 2};
        constexpr int widths[]        = {1920, 1280, 2560, 3840};
        constexpr int heights[]       = {1080, 720, 1440, 2160};
        constexpr int fpsValues[]     = {30, 60, 120};
        constexpr int bitrateValues[] = {10, 20, 40};

        ImGui::TextUnformatted("playback.refactorEditor.export.title"_tr().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.subtitle"_tr().c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted("playback.refactorEditor.export.output"_tr().c_str());
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.name"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##export-name", mExportName.data(), mExportName.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.location"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##export-directory", mExportDirectory.data(), mExportDirectory.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.format"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-format", &mFormatPreset, formatOptions, IM_ARRAYSIZE(formatOptions)))
            mCodecPreset = 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.codec"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("##export-codec", &mCodecPreset, codecOptions[mFormatPreset], codecCounts[mFormatPreset]);

        ImGui::Spacing();
        ImGui::TextUnformatted("playback.refactorEditor.export.video"_tr().c_str());
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.aspect"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-aspect", &mAspectPreset, aspectOptions, IM_ARRAYSIZE(aspectOptions))
            && mAspectPreset != 3) {
            float aspect = mAspectPreset == 0 ? 16.0f / 9.0f : mAspectPreset == 1 ? 9.0f / 16.0f : 1.0f;
            mHeight      = std::max(1, static_cast<int>(mWidth / aspect + 0.5f));
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.resolution"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-resolution", &mResolutionPreset, resolutionOptions, IM_ARRAYSIZE(resolutionOptions))
            && mResolutionPreset < 4) {
            mWidth  = widths[mResolutionPreset];
            mHeight = heights[mResolutionPreset];
            if (mAspectPreset == 1) std::swap(mWidth, mHeight);
            if (mAspectPreset == 2) mHeight = mWidth;
        }
        if (mResolutionPreset == 4) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            ImGui::InputInt("##export-width", &mWidth);
            ImGui::SameLine();
            ImGui::TextUnformatted("x");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            ImGui::InputInt("##export-height", &mHeight);
            mWidth  = std::clamp(mWidth, 16, 7680);
            mHeight = std::clamp(mHeight, 16, 4320);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.frameRate"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-fps", &mFpsPreset, fpsOptions, IM_ARRAYSIZE(fpsOptions)) && mFpsPreset < 3)
            mFps = fpsValues[mFpsPreset];
        if (mFpsPreset == 3) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            ImGui::InputInt("##export-custom-fps", &mFps);
            mFps = std::clamp(mFps, 1, 240);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.bitrate"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-bitrate", &mBitratePreset, bitrateOptions, IM_ARRAYSIZE(bitrateOptions))
            && mBitratePreset < 3)
            mBitrateMbps = bitrateValues[mBitratePreset];
        if (mBitratePreset == 3) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            ImGui::InputInt("##export-custom-bitrate", &mBitrateMbps);
            ImGui::SameLine();
            ImGui::TextUnformatted("Mbps");
            mBitrateMbps = std::clamp(mBitrateMbps, 1, 200);
        }

        ImGui::Spacing();
        ImGui::Separator();
        char        summary[192];
        const char* extensions[] = {"mp4", "mkv", "webm", "mov", "avi"};
        std::snprintf(
            summary,
            sizeof(summary),
            "%dx%d  |  %d FPS  |  %d Mbps  |  %s / %s",
            mWidth,
            mHeight,
            mFps,
            mBitrateMbps,
            formatOptions[mFormatPreset],
            codecOptions[mFormatPreset][mCodecPreset]
        );
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.summary"_tr().c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(summary);
        std::string const outputFile =
            std::string(mExportDirectory.data()) + "/" + mExportName.data() + "." + extensions[mFormatPreset];
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.file"_tr(outputFile).c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled();
        ImGui::Button("playback.refactorEditor.export.export"_tr().c_str(), ImVec2(140.0f, 32.0f));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", "playback.refactorEditor.export.backendUnavailable"_tr().c_str());
        ImGui::SameLine();
        if (ImGui::Button("playback.refactorEditor.export.cancel"_tr().c_str(), ImVec2(110.0f, 32.0f))) {
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool EditorMenuBar::isAnyMenuOpen() const { return ImGui::IsAnyItemHovered(); }

} // namespace playback::editor::ui
