#include "EditorMenuBar.h"

#include "playback/editor/exporting/ExportPlanCompiler.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cstdint>
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

bool inputClampedInt(char const* id, int& value, int minimum, int maximum, float width) {
    ImGui::SetNextItemWidth(width);
    bool const changed = ImGui::InputInt(
        id,
        &value,
        1,
        10,
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
    );
    if (ImGui::IsItemDeactivatedAfterEdit() || (changed && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        value = std::clamp(value, minimum, maximum);
    }
    return changed;
}

void shortcutRow(std::string const& keys, std::string const& description) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", keys.c_str());
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(description.c_str());
}

std::string shortcutPair(input::EditorKeybind first, input::EditorKeybind second) {
    return input::KeyMap::displayString(first) + " / " + input::KeyMap::displayString(second);
}

} // namespace

void EditorMenuBar::openExportDialog(int totalTicks, bool ffmpegAvailable) {
    totalTicks = std::max(0, totalTicks);
    if (!mExportSettingsInitialized) {
        mExportStartTick           = 0;
        mExportEndTick             = totalTicks;
        mExportSettingsInitialized = true;
    } else {
        mExportStartTick = std::clamp(mExportStartTick, 0, totalTicks);
        mExportEndTick   = std::clamp(mExportEndTick, 0, totalTicks);
        if (mExportEndTick <= mExportStartTick) {
            mExportStartTick = 0;
            mExportEndTick   = totalTicks;
        }
    }
    if (!ffmpegAvailable && mExportFormat == 0) mExportFormat = 1;
    mExportDialogOpen = true;
}

void EditorMenuBar::draw() {
    auto&       editor       = ReplayEditor::getInstance();
    auto const& state        = editor.state();
    auto const& capabilities = state.capabilities;
    auto const  exportActive = exporting::isExportActive(state.exportStatus.state);
    auto const  exportShortcut = input::KeyMap::displayString(input::EditorKeybind::OpenExport);
    auto const  undoShortcut = input::KeyMap::displayString(input::EditorKeybind::Undo);
    auto const  redoShortcut = input::KeyMap::displayString(input::EditorKeybind::Redo);
    auto const  deleteShortcut = input::KeyMap::displayString(input::EditorKeybind::DeleteSelection);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("playback.refactorEditor.menu.file"_tr().c_str())) {
            ImGui::MenuItem(
                "playback.refactorEditor.menu.openReplay"_tr().c_str(),
                nullptr,
                false,
                false
            );
            ImGui::MenuItem(
                "playback.refactorEditor.menu.saveProject"_tr().c_str(),
                nullptr,
                false,
                false
            );
            ImGui::MenuItem("playback.refactorEditor.menu.recent"_tr().c_str(), nullptr, false, false);
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.export"_tr().c_str(),
                    exportShortcut.c_str(),
                    false,
                    capabilities.videoExport && !exportActive && state.totalTicks > 0
                )) {
                openExportDialog(state.totalTicks, capabilities.ffmpegVideoExport);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.menu.exit"_tr().c_str(), "Esc (hold)")) {
                editor.submitAction({playback::editor::EditorActionType::StopReplay});
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("playback.refactorEditor.menu.edit"_tr().c_str())) {
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.undo"_tr().c_str(),
                    undoShortcut.c_str(),
                    false,
                    state.canUndo
                )) {
                editor.submitAction({EditorActionType::UndoEditorEdit});
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.redo"_tr().c_str(),
                    redoShortcut.c_str(),
                    false,
                    state.canRedo
                )) {
                editor.submitAction({EditorActionType::RedoEditorEdit});
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.delete"_tr().c_str(),
                    deleteShortcut.c_str(),
                    false,
                    editor.selection().hasSelection()
                )) {
                (void)editor.deleteSelection();
            }
            ImGui::MenuItem("playback.refactorEditor.menu.selectAll"_tr().c_str(), "Ctrl+A", false, false);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("playback.refactorEditor.menu.camera"_tr().c_str(), capabilities.cameraEditing)) {
            auto const keyframeShortcut = input::KeyMap::displayString(input::EditorKeybind::AddKeyframe);
            bool const canAddKeyframe = editor.selection().getAs<editing::model::SelectedCamera>() != nullptr;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.addKeyframe"_tr().c_str(),
                    keyframeShortcut.c_str(),
                    false,
                    canAddKeyframe
                )) {
                (void)editor.addKeyframeAtPlayhead();
            }
            ImGui::EndMenu();
        }

        ImGui::MenuItem("playback.refactorEditor.menu.markers"_tr().c_str(), nullptr, false, capabilities.videoEditing);

        if (ImGui::BeginMenu("playback.refactorEditor.menu.window"_tr().c_str())) {
            ImGui::MenuItem("playback.refactorEditor.menu.hintBar"_tr().c_str(), "F1", false, false);
            ImGui::Separator();
            bool const curveOpen = editor.curveEditorPanel().isOpen();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.curveEditor"_tr().c_str(),
                    nullptr,
                    curveOpen,
                    capabilities.cameraEditing
                )) {
                editor.curveEditorPanel().setOpen(!curveOpen);
            }
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
    float const shortcutWidth = std::min(620.0f, std::max(420.0f, ImGui::GetMainViewport()->WorkSize.x - 32.0f));
    ImGui::SetNextWindowSize(ImVec2(shortcutWidth, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "##KeyboardShortcuts",
            &mShortcutDialogOpen,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
        )) {
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.title"_tr().c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.playback"_tr().c_str());
        if (ImGui::BeginTable("##playback-shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##playback-key", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("##playback-command", ImGuiTableColumnFlags_WidthStretch);
            shortcutRow(
                shortcutPair(input::EditorKeybind::JumpStart, input::EditorKeybind::JumpEnd),
                "playback.refactorEditor.shortcuts.jumpEdges"_tr()
            );
            shortcutRow(
                shortcutPair(input::EditorKeybind::SeekSecondLeft, input::EditorKeybind::SeekSecondRight),
                "playback.refactorEditor.shortcuts.seekSeconds"_tr()
            );
            shortcutRow(
                shortcutPair(input::EditorKeybind::SeekTickLeft, input::EditorKeybind::SeekTickRight),
                "playback.refactorEditor.shortcuts.seekTicks"_tr()
            );
            shortcutRow(
                shortcutPair(input::EditorKeybind::PreviousEditPoint, input::EditorKeybind::NextEditPoint),
                "playback.refactorEditor.shortcuts.editPoints"_tr()
            );
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.editing"_tr().c_str());
        if (ImGui::BeginTable("##editing-shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##editing-key", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("##editing-command", ImGuiTableColumnFlags_WidthStretch);
            shortcutRow(
                shortcutPair(input::EditorKeybind::Undo, input::EditorKeybind::Redo),
                "playback.refactorEditor.shortcuts.undoRedo"_tr()
            );
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::AddKeyframe),
                "playback.refactorEditor.shortcuts.keyframe"_tr()
            );
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::SplitAtPlayhead),
                "playback.refactorEditor.shortcuts.split"_tr()
            );
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::DeleteSelection),
                "playback.refactorEditor.shortcuts.delete"_tr()
            );
            shortcutRow(
                shortcutPair(input::EditorKeybind::ZoomOutTimeline, input::EditorKeybind::ZoomInTimeline),
                "playback.refactorEditor.shortcuts.zoom"_tr()
            );
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("playback.refactorEditor.shortcuts.application"_tr().c_str());
        if (ImGui::BeginTable("##application-shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##application-key", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("##application-command", ImGuiTableColumnFlags_WidthStretch);
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::OpenExport),
                "playback.refactorEditor.shortcuts.export"_tr()
            );
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::ToggleViewportMaximized),
                "playback.refactorEditor.shortcuts.viewport"_tr()
            );
            ImGui::EndTable();
        }
        ImGui::Spacing();
        float const closeWidth = 110.0f;
        ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, ImGui::GetWindowWidth() - closeWidth - ImGui::GetStyle().WindowPadding.x));
        if (ImGui::Button("playback.refactorEditor.shortcuts.close"_tr().c_str(), {closeWidth, 32.0f})) {
            mShortcutDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (mExportDialogOpen) ImGui::OpenPopup("##ExportVideo");
    ImVec2 const exportWorkSize = ImGui::GetMainViewport()->WorkSize;
    ImVec2 const exportDialogSize{
        std::max(1.0f, std::min(680.0f, exportWorkSize.x - 24.0f)),
        std::max(1.0f, std::min(720.0f, exportWorkSize.y - 24.0f))
    };
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(exportDialogSize, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##ExportVideo", &mExportDialogOpen, ImGuiWindowFlags_NoResize)) {
        std::string const custom            = "playback.refactorEditor.export.custom"_tr();
        char const*       fpsOptions[]      = {"30 FPS", "60 FPS", "120 FPS", custom.c_str()};
        constexpr int     fpsValues[]       = {30, 60, 120};
        char const*       ssaaOptions[]     = {"1x", "2x", "4x", "8x"};
        int const         maximumReplayTick = std::max(state.totalTicks, 0);
        float const       labelWidth        = std::clamp(exportDialogSize.x * 0.28f, 120.0f, 180.0f);

        std::string const title = std::string(ICON_EXPORT) + "  " + "playback.refactorEditor.export.title"_tr();
        ImGui::TextUnformatted(title.c_str());
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.subtitle"_tr().c_str());
        ImGui::Separator();

        float const footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        ImGui::BeginChild("##ExportSettingsBody", {0.0f, -footerHeight}, false);

        ImGui::TextDisabled("%s", "playback.refactorEditor.export.output"_tr().c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("##export-output", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##output-label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##output-value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.name"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##export-name", mExportName.data(), mExportName.size());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.location"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##export-directory", mExportDirectory.data(), mExportDirectory.size());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.format"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            std::string const mp4Format     = "playback.refactorEditor.export.mp4"_tr();
            std::string const pngFormat     = "playback.refactorEditor.export.pngSequence"_tr();
            char const*       formatPreview = mExportFormat == 0 ? mp4Format.c_str() : pngFormat.c_str();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##export-format", formatPreview)) {
                ImGui::BeginDisabled(!capabilities.ffmpegVideoExport);
                if (ImGui::Selectable(mp4Format.c_str(), mExportFormat == 0)) mExportFormat = 0;
                ImGui::EndDisabled();
                if (ImGui::Selectable(pngFormat.c_str(), mExportFormat == 1)) mExportFormat = 1;
                ImGui::EndCombo();
            }
            ImGui::EndTable();
        }
        if (!capabilities.ffmpegVideoExport) {
            ImGui::TextDisabled("%s  %s", ICON_INFO, "playback.refactorEditor.export.ffmpegUnavailable"_tr().c_str());
        }

        ImGui::Spacing();
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.timeline"_tr().c_str());
        ImGui::SameLine();
        std::string const fullRangeLabel = std::string(ICON_RESET) + " " + "playback.refactorEditor.export.fullRange"_tr();
        if (ImGui::SmallButton((fullRangeLabel + "##export-full-range").c_str())) {
            mExportStartTick = 0;
            mExportEndTick   = maximumReplayTick;
        }
        ImGui::Separator();
        if (ImGui::BeginTable("##export-timeline", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##timeline-label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##timeline-value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.frameRate"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(mFpsPreset == 3 ? 190.0f : -1.0f);
            if (ImGui::Combo("##export-fps", &mFpsPreset, fpsOptions, IM_ARRAYSIZE(fpsOptions)) && mFpsPreset < 3)
                mFps = fpsValues[mFpsPreset];
            if (mFpsPreset == 3) {
                ImGui::SameLine();
                inputClampedInt("##export-custom-fps", mFps, 1, 240, 100.0f);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.startTick"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            inputClampedInt("##export-start-tick", mExportStartTick, 0, maximumReplayTick, 150.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string(ICON_CLOCK) + "##export-start-current").c_str()))
                mExportStartTick = std::clamp(state.currentTick, 0, maximumReplayTick);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", "playback.refactorEditor.export.usePlayhead"_tr().c_str());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.endTick"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            inputClampedInt("##export-end-tick", mExportEndTick, 0, maximumReplayTick, 150.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string(ICON_CLOCK) + "##export-end-current").c_str()))
                mExportEndTick = std::clamp(state.currentTick, 0, maximumReplayTick);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", "playback.refactorEditor.export.usePlayhead"_tr().c_str());
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.capture"_tr().c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("##export-capture", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##capture-label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##capture-value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.resolution"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            inputClampedInt("##export-width", mExportWidth, 16, 16384, 120.0f);
            ImGui::SameLine();
            ImGui::TextUnformatted("x");
            ImGui::SameLine();
            inputClampedInt("##export-height", mExportHeight, 16, 16384, 120.0f);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.ssaa"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("##export-ssaa", &mExportSsaa, ssaaOptions, IM_ARRAYSIZE(ssaaOptions)))
                mExportSsaa = std::clamp(mExportSsaa, 0, 3);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("playback.refactorEditor.export.warmupFrames"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            inputClampedInt("##export-warmup", mExportWarmupFrames, 0, 3600, 150.0f);
            ImGui::EndTable();
        }

        int const      safeSsaaIndex = std::clamp(mExportSsaa, 0, 3);
        uint32_t const ssaaValue     = 1u << safeSsaaIndex;
        bool const     validOutput   = mExportName.front() != '\0' && mExportDirectory.front() != '\0';
        bool const     validTimeline = mExportStartTick >= 0 && mExportEndTick > mExportStartTick
                                && mExportEndTick <= maximumReplayTick;
        bool const validFps = mFps >= 1 && mFps <= 240;
        bool const validResolution = mExportWidth >= 16 && mExportHeight >= 16 && mExportWidth <= 16384
                                  && mExportHeight <= 16384
                                  && static_cast<uint64_t>(mExportWidth) * ssaaValue <= 16384
                                  && static_cast<uint64_t>(mExportHeight) * ssaaValue <= 16384
                                  && static_cast<uint64_t>(mExportWidth) * mExportHeight <= 134217728ull
                                  && static_cast<uint64_t>(mExportWidth) * mExportHeight * ssaaValue * ssaaValue
                                         <= 134217728ull;
        bool const validCapture = mExportSsaa >= 0 && mExportSsaa <= 3 && mExportWarmupFrames >= 0
                               && mExportWarmupFrames <= 3600;
        bool const formatAvailable = mExportFormat != 0 || capabilities.ffmpegVideoExport;
        bool const rawSettingsValid = validOutput && validTimeline && validFps && validResolution && validCapture
                                   && formatAvailable && state.project != nullptr;

        exporting::ExportSettings previewSettings;
        previewSettings.outputDirectory = utf8Path(mExportDirectory.data());
        previewSettings.outputName      = mExportName.data();
        previewSettings.startTick       = mExportStartTick;
        previewSettings.endTick         = mExportEndTick;
        previewSettings.frameRate       = {mFps, 1};
        previewSettings.resolutionX     = static_cast<uint32_t>(std::max(0, mExportWidth));
        previewSettings.resolutionY     = static_cast<uint32_t>(std::max(0, mExportHeight));
        previewSettings.ssaa            = ssaaValue;
        previewSettings.warmupFrames    = static_cast<uint32_t>(std::max(0, mExportWarmupFrames));
        previewSettings.format =
            mExportFormat == 0 ? exporting::ExportFormat::Mp4Video : exporting::ExportFormat::PngSequence;

        exporting::ExportPlanCompileResult compiled;
        if (rawSettingsValid) compiled = exporting::ExportPlanCompiler::compile(previewSettings, *state.project);
        bool const validSettings = rawSettingsValid && static_cast<bool>(compiled);
        auto outputPath = utf8String(
            compiled.plan ? compiled.plan->outputPath : exporting::buildExportOutputPath(previewSettings)
        );
        double const durationSeconds = validTimeline ? (mExportEndTick - mExportStartTick) / 20.0 : 0.0;
        uint64_t const frameCount = compiled.plan ? compiled.plan->frameCount : 0;

        ImGui::Spacing();
        ImGui::TextDisabled("%s", "playback.refactorEditor.export.summary"_tr().c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("##export-summary", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##summary-label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##summary-value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", "playback.refactorEditor.export.timelineSummary"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(
                "playback.refactorEditor.export.summaryValue"_tr(mFps, durationSeconds, frameCount).c_str()
            );
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", "playback.refactorEditor.export.captureSummary"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(
                "playback.refactorEditor.export.captureValue"_tr(
                    mExportWidth,
                    mExportHeight,
                    ssaaValue,
                    mExportWarmupFrames
                )
                    .c_str()
            );
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", "playback.refactorEditor.export.destinationLabel"_tr().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText(
                "##export-destination",
                outputPath.data(),
                outputPath.size() + 1,
                ImGuiInputTextFlags_ReadOnly
            );
            ImGui::EndTable();
        }

        if (!validSettings) {
            std::string validationMessage;
            if (!validOutput) validationMessage = "playback.refactorEditor.export.invalidOutput"_tr();
            else if (!validTimeline) validationMessage = "playback.refactorEditor.export.invalidTimeline"_tr();
            else if (!validFps) validationMessage = "playback.refactorEditor.export.invalidFps"_tr();
            else if (!validResolution || !validCapture)
                validationMessage = "playback.refactorEditor.export.invalidCapture"_tr();
            else if (!formatAvailable)
                validationMessage = "playback.refactorEditor.export.ffmpegUnavailable"_tr();
            else validationMessage = "playback.refactorEditor.export.invalidSettings"_tr();
            ImGui::TextColored(ImVec4(0.96f, 0.65f, 0.20f, 1.0f), "%s  %s", ICON_WARNING, validationMessage.c_str());
            if (!compiled.message.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", compiled.message.c_str());
        }

        ImGui::EndChild();
        ImGui::Separator();

        std::string const cancelLabel = std::string(ICON_CLOSE) + "  " + "playback.refactorEditor.export.cancel"_tr();
        std::string const startLabel  = std::string(ICON_EXPORT) + "  " + "playback.refactorEditor.export.export"_tr();
        float const       cancelWidth = 120.0f;
        float const       startWidth  = 170.0f;
        float const       footerWidth = cancelWidth + ImGui::GetStyle().ItemSpacing.x + startWidth;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - footerWidth - ImGui::GetStyle().WindowPadding.x));
        if (ImGui::Button(cancelLabel.c_str(), {cancelWidth, 32.0f})) {
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!validSettings || exportActive);
        if (ImGui::Button(startLabel.c_str(), {startWidth, 32.0f})) {
            EditorAction action{EditorActionType::StartExport};
            action.exportSettings = std::move(previewSettings);
            editor.submitAction(std::move(action));
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool EditorMenuBar::isAnyMenuOpen() const { return ImGui::IsAnyItemHovered(); }

} // namespace playback::editor::ui
