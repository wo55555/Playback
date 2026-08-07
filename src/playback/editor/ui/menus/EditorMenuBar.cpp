#include "EditorMenuBar.h"

#include "playback/editor/exporting/ExportPlanCompiler.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

[[nodiscard]] std::filesystem::path utf8Path(std::string const& value) {
    auto const* begin = reinterpret_cast<char8_t const*>(value.data());
    return std::filesystem::path(std::u8string{begin, begin + value.size()});
}

[[nodiscard]] std::string utf8String(std::filesystem::path const& value) {
    auto const text = value.generic_u8string();
    return {reinterpret_cast<char const*>(text.data()), text.size()};
}

} // namespace

void EditorMenuBar::draw() {
    auto&       editor       = ReplayEditor::getInstance();
    auto const& state        = editor.state();
    auto const& capabilities = state.capabilities;
    auto const  exportActive = exporting::isExportActive(state.exportStatus.state);
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
                    capabilities.videoExport && !exportActive && state.totalTicks > 0
                )) {
                mExportStartTick = 0;
                mExportEndTick   = state.totalTicks;
                if (!capabilities.ffmpegVideoExport && mExportFormat == 0) mExportFormat = 1;
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
        ImVec2(560.0f, 0.0f),
        ImVec2(560.0f, ImGui::GetMainViewport()->WorkSize.y - 24.0f)
    );
    if (ImGui::BeginPopupModal(
            "##ExportVideo",
            &mExportDialogOpen,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
        )) {
        std::string const custom       = "playback.refactorEditor.export.custom"_tr();
        const char*       fpsOptions[] = {"30 FPS", "60 FPS", "120 FPS", custom.c_str()};
        constexpr int     fpsValues[]  = {30, 60, 120};

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
        std::string const mp4Format     = "playback.refactorEditor.export.mp4"_tr();
        std::string const pngFormat     = "playback.refactorEditor.export.pngSequence"_tr();
        char const*       formatPreview = mExportFormat == 0 ? mp4Format.c_str() : pngFormat.c_str();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##export-format", formatPreview)) {
            bool const mp4Selected = mExportFormat == 0;
            ImGui::BeginDisabled(!capabilities.ffmpegVideoExport);
            if (ImGui::Selectable(mp4Format.c_str(), mp4Selected)) mExportFormat = 0;
            ImGui::EndDisabled();
            if (ImGui::Selectable(pngFormat.c_str(), mExportFormat == 1)) mExportFormat = 1;
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("playback.refactorEditor.export.timeline"_tr().c_str());
        ImGui::Separator();
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
        ImGui::TextUnformatted("playback.refactorEditor.export.startTick"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("##export-start-tick", &mExportStartTick);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("playback.refactorEditor.export.endTick"_tr().c_str());
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("##export-end-tick", &mExportEndTick);
        mExportStartTick = std::clamp(mExportStartTick, 0, state.totalTicks);
        mExportEndTick   = std::clamp(mExportEndTick, 0, state.totalTicks);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.summary"_tr().c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(
            "playback.refactorEditor.export.summaryValue"_tr(mFps, mExportStartTick, mExportEndTick).c_str()
        );
        exporting::ExportSettings previewSettings;
        previewSettings.outputDirectory = utf8Path(mExportDirectory.data());
        previewSettings.outputName      = mExportName.data();
        previewSettings.startTick       = mExportStartTick;
        previewSettings.endTick         = mExportEndTick;
        previewSettings.frameRate       = {mFps, 1};
        previewSettings.format =
            mExportFormat == 0 ? exporting::ExportFormat::Mp4Video : exporting::ExportFormat::PngSequence;
        auto const outputPath = utf8String(exporting::buildExportOutputPath(previewSettings));
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.destination"_tr(outputPath).c_str());
        ImGui::Spacing();
        bool const validSettings = mExportName.front() != '\0' && mExportDirectory.front() != '\0'
                                && mExportEndTick > mExportStartTick && mFps > 0
                                && (mExportFormat != 0 || capabilities.ffmpegVideoExport);
        ImGui::BeginDisabled(!validSettings || exportActive);
        if (ImGui::Button("playback.refactorEditor.export.export"_tr().c_str(), ImVec2(140.0f, 32.0f))) {
            auto const                outputPathUtf8 = std::string(mExportDirectory.data());
            exporting::ExportSettings settings;
            settings.outputDirectory = utf8Path(outputPathUtf8);
            settings.outputName      = mExportName.data();
            settings.startTick       = mExportStartTick;
            settings.endTick         = mExportEndTick;
            settings.frameRate       = {mFps, 1};
            settings.format =
                mExportFormat == 0 ? exporting::ExportFormat::Mp4Video : exporting::ExportFormat::PngSequence;

            EditorAction action{EditorActionType::StartExport};
            action.exportSettings = std::move(settings);
            editor.submitAction(std::move(action));
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
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
