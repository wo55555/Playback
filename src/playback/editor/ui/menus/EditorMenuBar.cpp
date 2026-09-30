#include "EditorMenuBar.h"

#include "playback/editor/input/KeyMap.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/exporting/ExportPlanCompiler.h"
#include "playback/exporting/RenderDiagnostics.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

constexpr int DefaultRayTracedConvergenceFrames = 8;

// Matches the recorder's replay naming so exports and recordings sort together.
[[nodiscard]] std::string timestampName() {
    auto const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm    local{};
    if (localtime_s(&local, &now) != 0) return "export";
    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H-%M-%S", &local) == 0) return "export";
    return buffer.data();
}

[[nodiscard]] std::filesystem::path utf8Path(std::string const& value) {
    auto const* begin = reinterpret_cast<char8_t const*>(value.data());
    return std::filesystem::path(std::u8string{begin, begin + value.size()});
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

// Dim caption with a hairline underneath, used for every group in the export dialog.
void sectionHeader(char const* label) {
    ImGui::Dummy({0.0f, 5.0f * metrics::scale()});
    ImGui::TextDisabled("%s", label);
    ImVec2 const min   = ImGui::GetItemRectMin();
    float const  y     = ImGui::GetItemRectMax().y + 3.0f * metrics::scale();
    float const  right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine({min.x, y}, {right, y}, theme::kBorder);
    ImGui::Dummy({0.0f, 3.0f * metrics::scale()});
}

bool beginPropertyTable(char const* id, float labelWidth) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void propertyLabel(char const* label, char const* hint = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (hint != nullptr) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextDisabled("(?)");
        widgets::itemTooltip(hint);
    }
    ImGui::TableSetColumnIndex(1);
}

// Row of mutually exclusive choices; the active one is filled with the accent.
bool segmented(char const* id, char const* const* options, int count, int& index, float width, int disabledMask = 0) {
    bool        changed = false;
    float const w       = width / static_cast<float>(count);
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1.0f, ImGui::GetStyle().ItemSpacing.y});
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        bool const active   = i == index;
        bool const disabled = (disabledMask >> i) & 1;
        ImGui::BeginDisabled(disabled);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::kAccent : theme::kButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? theme::kAccentHover : theme::kButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::kIconHighlight : theme::kText);
        if (ImGui::Button(options[i], {w, 0.0f}) && !active) {
            index   = i;
            changed = true;
        }
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

// Frame-height square icon button so it sits level with the input beside it.
bool frameIconButton(char const* id, char const* icon, char const* tooltip) {
    float const  size     = ImGui::GetFrameHeight();
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    bool const   clicked  = ImGui::InvisibleButton(id, {size, size});
    bool const   hovered  = ImGui::IsItemHovered();
    auto*        drawList = ImGui::GetWindowDrawList();
    ImU32 const  fill = ImGui::IsItemActive() ? theme::kButtonActive : hovered ? theme::kButtonHover : theme::kButton;
    drawList->AddRectFilled(origin, {origin.x + size, origin.y + size}, fill, theme::kFrameRounding);
    widgets::drawIconCentred(drawList, icon, origin, size, hovered ? theme::kIconHighlight : theme::kIconInactive);
    widgets::itemTooltip(tooltip);
    return clicked;
}

struct ResolutionPreset {
    int width;
    int height;
};
constexpr ResolutionPreset kResolutionPresets[] = {
    {1280, 720 },
    {1920, 1080},
    {2560, 1440},
    {3840, 2160}
};

// Dim note on the same row, right-aligned to `rightEdge` (screen x) so it lines up with the full-width fields.
void trailingText(char const* text, float rightEdge) {
    ImGui::SameLine();
    float const x = rightEdge - ImGui::GetStyle().FramePadding.x - ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, x - ImGui::GetCursorScreenPos().x));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
}

// Slim bar showing the chosen range against the whole replay, with the playhead marked.
void rangePreview(int start, int end, int current, int total, float width) {
    float const  h     = 6.0f * metrics::scale();
    float const  w     = std::max(1.0f, width);
    float const  slotH = ImGui::GetFrameHeight();
    ImVec2 const slot  = ImGui::GetCursorScreenPos();
    ImGui::Dummy({w, slotH});
    // Occupies a frame-height slot so it lines up with the button sharing its row.
    ImVec2 const min{slot.x, slot.y + (slotH - h) * 0.5f};
    auto*        drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, {min.x + w, min.y + h}, theme::kInputBg, h * 0.5f);
    if (total <= 0) return;
    auto const at = [&](int tick) {
        return min.x + w * static_cast<float>(std::clamp(tick, 0, total)) / static_cast<float>(total);
    };
    if (end > start) drawList->AddRectFilled({at(start), min.y}, {at(end), min.y + h}, theme::kAccent, h * 0.5f);
    float const px = at(current);
    drawList->AddLine({px, min.y - 2.0f}, {px, min.y + h + 2.0f}, theme::kPlayhead, 2.0f);
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
    // The dialog always edits a concrete range, so an unset marker resolves to the replay bounds here.
    if (mExportStartTick < 0 || mExportEndTick < 0 || mExportEndTick <= mExportStartTick) {
        mExportStartTick = 0;
        mExportEndTick   = totalTicks;
    } else {
        mExportStartTick = std::clamp(mExportStartTick, 0, totalTicks);
        mExportEndTick   = std::clamp(mExportEndTick, 0, totalTicks);
        if (mExportEndTick <= mExportStartTick) {
            mExportStartTick = 0;
            mExportEndTick   = totalTicks;
        }
    }
    if (!mExportSettingsInitialized) {
        mExportConvergenceFrames = exporting::rayTracingActive() ? DefaultRayTracedConvergenceFrames : 0;
        auto const stamp         = timestampName();
        std::snprintf(mExportName.data(), mExportName.size(), "%s", stamp.c_str());
    }
    mExportSettingsInitialized = true;
    if (!ffmpegAvailable && mExportFormat == 0) mExportFormat = 1;
    mExportDialogOpen = true;
}

void EditorMenuBar::markExportPoint(bool isIn, int tick, int totalTicks) {
    totalTicks = std::max(0, totalTicks);
    tick       = std::clamp(tick, 0, totalTicks);
    int start  = mExportStartTick < 0 ? 0 : mExportStartTick;
    int end    = mExportEndTick < 0 ? totalTicks : mExportEndTick;
    if (isIn) start = std::min(tick, end);
    else end = std::max(tick, start);

    // A range covering the whole replay is the same as having no range at all.
    if (start <= 0 && end >= totalTicks) {
        mExportStartTick = -1;
        mExportEndTick   = -1;
        return;
    }
    mExportStartTick = start;
    mExportEndTick   = end;
}

void EditorMenuBar::draw(PanelContext const& ctx) {
    drawMenus(ctx);
    drawShortcutDialog();
    drawExportDialog(ctx);
}

void EditorMenuBar::drawMenus(PanelContext const& ctx) {
    auto const& state          = ctx.state;
    auto const& capabilities   = state.capabilities;
    auto const  exportActive   = exporting::isExportActive(state.exportStatus.state);
    auto const  exportShortcut = input::KeyMap::displayString(input::EditorKeybind::OpenExport);
    auto const  undoShortcut   = input::KeyMap::displayString(input::EditorKeybind::Undo);
    auto const  redoShortcut   = input::KeyMap::displayString(input::EditorKeybind::Redo);
    auto const  deleteShortcut = input::KeyMap::displayString(input::EditorKeybind::DeleteSelection);
    auto const  saveShortcut   = input::KeyMap::displayString(input::EditorKeybind::SaveProject);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("playback.refactorEditor.menu.file"_tr().c_str())) {
            ImGui::MenuItem("playback.refactorEditor.menu.openReplay"_tr().c_str(), nullptr, false, false);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.saveProject"_tr().c_str(),
                    saveShortcut.c_str(),
                    false,
                    state.editorVisible && !exportActive
                )) {
                ctx.submitAction({EditorActionType::SaveProject});
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.reloadProject"_tr().c_str(),
                    nullptr,
                    false,
                    state.editorVisible && !exportActive && !state.persistence.projectFile.empty()
                )) {
                ctx.submitAction({EditorActionType::LoadProject});
            }
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
                ctx.submitAction({playback::state::EditorActionType::StopReplay});
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
                ctx.submitAction({EditorActionType::UndoEditorEdit});
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.redo"_tr().c_str(),
                    redoShortcut.c_str(),
                    false,
                    state.canRedo
                )) {
                ctx.submitAction({EditorActionType::RedoEditorEdit});
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.delete"_tr().c_str(),
                    deleteShortcut.c_str(),
                    false,
                    ctx.selection.hasSelection()
                )) {
                (void)ctx.commands.deleteSelection();
            }
            ImGui::MenuItem("playback.refactorEditor.menu.selectAll"_tr().c_str(), "Ctrl+A", false, false);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("playback.refactorEditor.menu.view"_tr().c_str())) {
            bool const viewEnabled = state.editorVisible && !exportActive;
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.cameraPath"_tr().c_str(),
                    nullptr,
                    ctx.commands.isCameraPathVisible(),
                    viewEnabled
                )) {
                ctx.commands.toggleCameraPath();
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.viewport.infoOverlay"_tr().c_str(),
                    nullptr,
                    ctx.commands.isInfoOverlayVisible(),
                    viewEnabled
                )) {
                ctx.commands.setInfoOverlayVisible(!ctx.commands.isInfoOverlayVisible());
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.viewport.autoPreview"_tr().c_str(),
                    nullptr,
                    ctx.commands.isAutoPreviewEnabled(),
                    viewEnabled
                )) {
                ctx.commands.setAutoPreviewEnabled(!ctx.commands.isAutoPreviewEnabled());
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.snap"_tr().c_str(),
                    nullptr,
                    ctx.commands.isSnapEnabled(),
                    viewEnabled
                )) {
                ctx.commands.setSnapEnabled(!ctx.commands.isSnapEnabled());
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.maximize"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::ToggleViewportMaximized).c_str(),
                    ctx.commands.isViewportMaximized(),
                    viewEnabled
                )) {
                ctx.commands.toggleViewportMaximized();
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
}

void EditorMenuBar::drawShortcutDialog() {
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
                input::KeyMap::displayString(input::EditorKeybind::TogglePlayback),
                "playback.refactorEditor.shortcuts.playPause"_tr()
            );
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
            shortcutRow(
                shortcutPair(input::EditorKeybind::DecreaseSpeed, input::EditorKeybind::IncreaseSpeed),
                "playback.refactorEditor.shortcuts.speed"_tr()
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
                input::KeyMap::displayString(input::EditorKeybind::SaveProject),
                "playback.refactorEditor.shortcuts.saveProject"_tr()
            );
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::AddKeyframe),
                "playback.refactorEditor.shortcuts.keyframe"_tr()
            );
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::AddCameraTrack),
                "playback.refactorEditor.shortcuts.addCamera"_tr()
            );
            shortcutRow("1 / 2 / 3", "playback.refactorEditor.shortcuts.selectCamera"_tr());
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::DeleteSelection),
                "playback.refactorEditor.shortcuts.delete"_tr()
            );
            shortcutRow(
                shortcutPair(input::EditorKeybind::ZoomOutTimeline, input::EditorKeybind::ZoomInTimeline),
                "playback.refactorEditor.shortcuts.zoom"_tr()
            );
            shortcutRow("Alt+Wheel", "playback.refactorEditor.shortcuts.zoomWheel"_tr());
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::ResetTimelineZoom),
                "playback.refactorEditor.shortcuts.zoomReset"_tr()
            );
            shortcutRow(
                shortcutPair(input::EditorKeybind::MarkExportIn, input::EditorKeybind::MarkExportOut),
                "playback.refactorEditor.shortcuts.exportRange"_tr()
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
            shortcutRow(
                shortcutPair(input::EditorKeybind::DecreaseUiScale, input::EditorKeybind::IncreaseUiScale),
                "playback.refactorEditor.shortcuts.uiScale"_tr()
            );
            shortcutRow(
                input::KeyMap::displayString(input::EditorKeybind::ResetUiScale),
                "playback.refactorEditor.shortcuts.uiScaleReset"_tr()
            );
            ImGui::EndTable();
        }
        ImGui::Spacing();
        float const closeWidth = 110.0f;
        ImGui::SetCursorPosX(
            std::max(
                ImGui::GetStyle().WindowPadding.x,
                ImGui::GetWindowWidth() - closeWidth - ImGui::GetStyle().WindowPadding.x
            )
        );
        if (ImGui::Button("playback.refactorEditor.shortcuts.close"_tr().c_str(), {closeWidth, 32.0f})) {
            mShortcutDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorMenuBar::drawExportDialog(PanelContext const& ctx) {
    auto const& state        = ctx.state;
    auto const& capabilities = state.capabilities;
    auto const  exportActive = exporting::isExportActive(state.exportStatus.state);

    if (mExportDialogOpen) ImGui::OpenPopup("##ExportVideo");
    ImVec2 const exportWorkSize = ImGui::GetMainViewport()->WorkSize;
    float const  uiScale        = metrics::scale();
    ImVec2 const exportDialogSize{std::max(1.0f, std::min(580.0f * uiScale, exportWorkSize.x - 24.0f)), 0.0f};
    ImVec2 const dialogPadding{22.0f * uiScale, 14.0f * uiScale};
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Height fits the content and never changes; the dialog only scrolls when the viewport is shorter than that.
    ImGui::SetNextWindowSize(exportDialogSize, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        {exportDialogSize.x, 0.0f},
        {exportDialogSize.x, std::max(1.0f, exportWorkSize.y - 24.0f)}
    );
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, dialogPadding);
    bool const dialogVisible = ImGui::BeginPopupModal("##ExportVideo", &mExportDialogOpen, ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar();
    if (dialogVisible) {
        std::string const custom                   = "playback.refactorEditor.export.custom"_tr();
        char const*       fpsOptions[]             = {"30 FPS", "60 FPS", "120 FPS", custom.c_str()};
        constexpr int     fpsValues[]              = {30, 60, 120};
        char const*       ssaaOptions[]            = {"1x", "2x", "4x"};
        char const*       resolutionPresetLabels[] = {"720p", "1080p", "1440p", "4K"};
        static_assert(IM_ARRAYSIZE(resolutionPresetLabels) == IM_ARRAYSIZE(kResolutionPresets));
        std::string const mp4Format       = "playback.refactorEditor.export.mp4"_tr();
        std::string const pngFormat       = "playback.refactorEditor.export.pngSequence"_tr();
        char const*       formatOptions[] = {mp4Format.c_str(), pngFormat.c_str()};
        mExportSsaa                       = std::clamp(mExportSsaa, 0, 2);
        int const maximumReplayTick       = std::max(state.totalTicks, 0);
        // Fixed label column aligns every value; the capped value column stops inputs spanning the dialog.
        auto const& style = ImGui::GetStyle();
        // The label column must fit the longest label that carries a "(?)" hint.
        float hintedLabelWidth = 0.0f;
        for (std::string const& label :
             {"playback.refactorEditor.export.ssaa"_tr(),
              "playback.refactorEditor.export.warmupFrames"_tr(),
              "playback.refactorEditor.export.convergenceFrames"_tr()}) {
            hintedLabelWidth = std::max(hintedLabelWidth, ImGui::CalcTextSize(label.c_str()).x);
        }
        hintedLabelWidth       += style.ItemInnerSpacing.x + ImGui::CalcTextSize("(?)").x + style.CellPadding.x * 2.0f;
        float const labelWidth  = std::min(
            std::max(std::clamp(exportDialogSize.x * 0.26f, 96.0f * uiScale, 150.0f * uiScale), hintedLabelWidth),
            exportDialogSize.x * 0.4f
        );
        float const fieldWidth = 92.0f * uiScale;
        float const frameH     = ImGui::GetFrameHeight();
        float const valueWidth = std::min(
            320.0f * uiScale,
            exportDialogSize.x - dialogPadding.x * 2.0f - labelWidth - style.CellPadding.x * 4.0f
        );

        // Header: accent-tinted icon tile with title and subtitle stacked beside it.
        {
            float const  box    = metrics::iconButton();
            ImVec2 const origin = ImGui::GetCursorScreenPos();
            auto*        dl     = ImGui::GetWindowDrawList();
            dl->AddRectFilled(
                origin,
                {origin.x + box, origin.y + box},
                theme::withAlpha(theme::kAccent, 0x40),
                theme::kFrameRounding * 2.0f
            );
            widgets::drawIconCentred(dl, ICON_EXPORT, origin, box, theme::kAccentHover);
            std::string const title    = "playback.refactorEditor.export.title"_tr();
            std::string const subtitle = "playback.refactorEditor.export.subtitle"_tr();
            float const       textX    = origin.x + box + style.ItemSpacing.x * 3.0f;
            float const       lineH    = ImGui::GetFontSize();
            float const       gap      = 2.0f * uiScale;
            float const       topY     = origin.y + (box - lineH * 2.0f - gap) * 0.5f;
            dl->AddText({textX, topY}, theme::kText, title.c_str());
            dl->AddText({textX, topY + lineH + gap}, theme::kTextDim, subtitle.c_str());
            ImGui::Dummy({0.0f, box});
        }
        ImGui::Dummy({0.0f, 4.0f * uiScale});
        ImGui::Separator();

        float const buttonH = frameH + 6.0f * uiScale;
        // Taller cells give the rows room to breathe without widening the label column.
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {style.CellPadding.x, 2.0f * uiScale});

        sectionHeader("playback.refactorEditor.export.output"_tr().c_str());
        if (beginPropertyTable("##export-output", labelWidth)) {
            propertyLabel("playback.refactorEditor.export.name"_tr().c_str());
            ImGui::SetNextItemWidth(valueWidth);
            ImGui::InputText("##export-name", mExportName.data(), mExportName.size());

            propertyLabel("playback.refactorEditor.export.location"_tr().c_str());
            ImGui::SetNextItemWidth(valueWidth);
            ImGui::InputText("##export-directory", mExportDirectory.data(), mExportDirectory.size());

            propertyLabel("playback.refactorEditor.export.format"_tr().c_str());
            segmented(
                "##export-format",
                formatOptions,
                IM_ARRAYSIZE(formatOptions),
                mExportFormat,
                valueWidth,
                capabilities.ffmpegVideoExport ? 0 : 1
            );
            ImGui::EndTable();
        }

        sectionHeader("playback.refactorEditor.export.timeline"_tr().c_str());
        if (beginPropertyTable("##export-timeline", labelWidth)) {
            propertyLabel("playback.refactorEditor.export.frameRate"_tr().c_str());
            float const presetWidth = valueWidth - (mFpsPreset == 3 ? fieldWidth + style.ItemSpacing.x : 0.0f);
            if (segmented("##export-fps", fpsOptions, IM_ARRAYSIZE(fpsOptions), mFpsPreset, presetWidth)
                && mFpsPreset < 3)
                mFps = fpsValues[mFpsPreset];
            if (mFpsPreset == 3) {
                ImGui::SameLine();
                inputClampedInt("##export-custom-fps", mFps, 1, 240, fieldWidth);
            }

            std::string const usePlayhead = "playback.refactorEditor.export.usePlayhead"_tr();
            propertyLabel("playback.refactorEditor.export.startTick"_tr().c_str());
            float const tickRowRight = ImGui::GetCursorScreenPos().x + valueWidth;
            inputClampedInt("##export-start-tick", mExportStartTick, 0, maximumReplayTick, fieldWidth);
            ImGui::SameLine();
            if (frameIconButton("##export-start-current", ICON_CLOCK, usePlayhead.c_str()))
                mExportStartTick = std::clamp(state.currentTick, 0, maximumReplayTick);
            trailingText(widgets::formatTick(mExportStartTick).c_str(), tickRowRight);

            propertyLabel("playback.refactorEditor.export.endTick"_tr().c_str());
            inputClampedInt("##export-end-tick", mExportEndTick, 0, maximumReplayTick, fieldWidth);
            ImGui::SameLine();
            if (frameIconButton("##export-end-current", ICON_CLOCK, usePlayhead.c_str()))
                mExportEndTick = std::clamp(state.currentTick, 0, maximumReplayTick);
            trailingText(widgets::formatTick(mExportEndTick).c_str(), tickRowRight);

            // Range bar under the tick fields, with the reset-to-full-replay button at its end.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            rangePreview(
                mExportStartTick,
                mExportEndTick,
                state.currentTick,
                maximumReplayTick,
                valueWidth - frameH - style.ItemSpacing.x
            );
            ImGui::SameLine();
            if (frameIconButton(
                    "##export-full-range",
                    ICON_RESET,
                    "playback.refactorEditor.export.fullRange"_tr().c_str()
                )) {
                mExportStartTick = 0;
                mExportEndTick   = maximumReplayTick;
            }
            ImGui::EndTable();
        }

        sectionHeader("playback.refactorEditor.export.capture"_tr().c_str());
        if (beginPropertyTable("##export-capture", labelWidth)) {
            propertyLabel("playback.refactorEditor.export.resolution"_tr().c_str());
            inputClampedInt("##export-width", mExportWidth, 16, 16384, fieldWidth);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("x");
            ImGui::SameLine();
            inputClampedInt("##export-height", mExportHeight, 16, 16384, fieldWidth);

            // No preset is lit while the fields hold a custom size.
            propertyLabel("playback.refactorEditor.export.resolutionPreset"_tr().c_str());
            int resolutionPreset = -1;
            for (int i = 0; i < IM_ARRAYSIZE(kResolutionPresets); ++i) {
                if (kResolutionPresets[i].width == mExportWidth && kResolutionPresets[i].height == mExportHeight)
                    resolutionPreset = i;
            }
            if (segmented(
                    "##export-resolution-preset",
                    resolutionPresetLabels,
                    IM_ARRAYSIZE(resolutionPresetLabels),
                    resolutionPreset,
                    fieldWidth * 2.0f + style.ItemSpacing.x
                )) {
                mExportWidth  = kResolutionPresets[resolutionPreset].width;
                mExportHeight = kResolutionPresets[resolutionPreset].height;
            }

            propertyLabel(
                "playback.refactorEditor.export.ssaa"_tr().c_str(),
                "playback.refactorEditor.export.ssaaHint"_tr().c_str()
            );
            int ssaaDisabledMask = 0;
            for (int i = 0; i < IM_ARRAYSIZE(ssaaOptions); ++i) {
                if (!exporting::supersampleFits(
                        static_cast<uint32_t>(mExportWidth),
                        static_cast<uint32_t>(mExportHeight),
                        1u << i
                    ))
                    ssaaDisabledMask |= 1 << i;
            }
            // Shrinking the output can strand the selection on a level that no longer fits.
            while (mExportSsaa > 0 && ((ssaaDisabledMask >> mExportSsaa) & 1) != 0) --mExportSsaa;
            if (segmented(
                    "##export-ssaa",
                    ssaaOptions,
                    IM_ARRAYSIZE(ssaaOptions),
                    mExportSsaa,
                    fieldWidth * 2.0f + style.ItemSpacing.x,
                    ssaaDisabledMask
                ))
                mExportSsaa = std::clamp(mExportSsaa, 0, 2);

            propertyLabel(
                "playback.refactorEditor.export.warmupFrames"_tr().c_str(),
                "playback.refactorEditor.export.warmupHint"_tr().c_str()
            );
            inputClampedInt("##export-warmup", mExportWarmupFrames, 0, 3600, fieldWidth);

            propertyLabel(
                "playback.refactorEditor.export.convergenceFrames"_tr().c_str(),
                "playback.refactorEditor.export.convergenceHint"_tr().c_str()
            );
            inputClampedInt("##export-convergence", mExportConvergenceFrames, 0, 240, fieldWidth);
            ImGui::EndTable();
        }

        int const      safeSsaaIndex = std::clamp(mExportSsaa, 0, 2);
        uint32_t const ssaaValue     = 1u << safeSsaaIndex;
        bool const     validOutput   = mExportName.front() != '\0' && mExportDirectory.front() != '\0';
        bool const     validTimeline =
            mExportStartTick >= 0 && mExportEndTick > mExportStartTick && mExportEndTick <= maximumReplayTick;
        bool const validFps         = mFps >= 1 && mFps <= 240;
        bool const validResolution  = mExportWidth >= 16 && mExportHeight >= 16
                                   && static_cast<uint32_t>(mExportWidth) <= exporting::MaxExportResolution
                                   && static_cast<uint32_t>(mExportHeight) <= exporting::MaxExportResolution
                                   && static_cast<uint64_t>(mExportWidth) * mExportHeight <= exporting::MaxExportPixels
                                   && exporting::supersampleFits(
                                          static_cast<uint32_t>(mExportWidth),
                                          static_cast<uint32_t>(mExportHeight),
                                          ssaaValue
                                  );
        bool const validCapture     = mExportSsaa >= 0 && mExportSsaa <= 2 && mExportWarmupFrames >= 0
                                   && mExportWarmupFrames <= 3600 && mExportConvergenceFrames >= 0
                                   && mExportConvergenceFrames <= 240;
        bool const formatAvailable  = mExportFormat != 0 || capabilities.ffmpegVideoExport;
        bool const rawSettingsValid = validOutput && validTimeline && validFps && validResolution && validCapture
                                   && formatAvailable && state.project != nullptr;

        exporting::ExportSettings previewSettings;
        previewSettings.outputDirectory   = utf8Path(mExportDirectory.data());
        previewSettings.outputName        = mExportName.data();
        previewSettings.startTick         = mExportStartTick;
        previewSettings.endTick           = mExportEndTick;
        previewSettings.frameRate         = {mFps, 1};
        previewSettings.resolutionX       = static_cast<uint32_t>(std::max(0, mExportWidth));
        previewSettings.resolutionY       = static_cast<uint32_t>(std::max(0, mExportHeight));
        previewSettings.ssaa              = ssaaValue;
        previewSettings.warmupFrames      = static_cast<uint32_t>(std::max(0, mExportWarmupFrames));
        previewSettings.convergenceFrames = static_cast<uint32_t>(std::max(0, mExportConvergenceFrames));
        previewSettings.format =
            mExportFormat == 0 ? exporting::ExportFormat::Mp4Video : exporting::ExportFormat::PngSequence;

        exporting::ExportPlanCompileResult compiled;
        if (rawSettingsValid) compiled = exporting::ExportPlanCompiler::compile(previewSettings, *state.project);
        bool const     validSettings   = rawSettingsValid && static_cast<bool>(compiled);
        double const   durationSeconds = validTimeline ? (mExportEndTick - mExportStartTick) / 20.0 : 0.0;
        uint64_t const frameCount      = compiled.plan ? compiled.plan->frameCount : 0;

        // Summary card: a tinted panel so the derived values read apart from the editable fields above.
        sectionHeader("playback.refactorEditor.export.summary"_tr().c_str());
        {
            std::string const timelineValue =
                "playback.refactorEditor.export.summaryValue"_tr(mFps, durationSeconds, frameCount);
            std::string const captureValue = "playback.refactorEditor.export.captureValue"_tr(
                mExportWidth,
                mExportHeight,
                ssaaValue,
                mExportWarmupFrames
            );
            // Shows the name the plan resolved, so a PNG sequence reads as a folder and MP4 as a file.
            std::string outputValue = "-";
            if (compiled.plan) {
                auto const u8 = compiled.plan->outputPath.filename().u8string();
                outputValue.assign(reinterpret_cast<char const*>(u8.data()), u8.size());
            }
            float const  pad     = 10.0f * uiScale;
            float const  lineH   = ImGui::GetFontSize() + 4.0f * uiScale;
            float const  cardH   = pad * 2.0f + lineH * 3.0f - 4.0f * uiScale;
            float const  cardW   = ImGui::GetContentRegionAvail().x;
            ImVec2 const cardMin = ImGui::GetCursorScreenPos();
            ImVec2 const cardMax{cardMin.x + cardW, cardMin.y + cardH};
            auto*        dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(cardMin, cardMax, theme::kSection, theme::kFrameRounding * 2.0f);
            dl->AddRect(cardMin, cardMax, theme::kBorder, theme::kFrameRounding * 2.0f);

            // The label column matches the tables above so the card reads as one continuous grid.
            float const  valueX = cardMin.x + labelWidth + style.CellPadding.x * 2.0f;
            ImVec2 const row0{cardMin.x + pad, cardMin.y + pad};
            dl->AddText(row0, theme::kTextDim, "playback.refactorEditor.export.timelineSummary"_tr().c_str());
            dl->AddText({valueX, row0.y}, theme::kText, timelineValue.c_str());
            dl->AddText(
                {row0.x, row0.y + lineH},
                theme::kTextDim,
                "playback.refactorEditor.export.captureSummary"_tr().c_str()
            );
            dl->AddText({valueX, row0.y + lineH}, theme::kText, captureValue.c_str());
            dl->AddText(
                {row0.x, row0.y + lineH * 2.0f},
                theme::kTextDim,
                "playback.refactorEditor.export.outputSummary"_tr().c_str()
            );
            // Clipped to the card so a long name cannot spill past the border.
            ImVec4 const clip{valueX, cardMin.y, cardMax.x - pad, cardMax.y};
            dl->AddText(
                nullptr,
                0.0f,
                {valueX, row0.y + lineH * 2.0f},
                theme::kText,
                outputValue.c_str(),
                nullptr,
                0.0f,
                &clip
            );
            ImGui::SetCursorScreenPos({cardMin.x, cardMax.y});
            ImGui::Dummy({cardW, style.ItemSpacing.y});
        }

        // Note slot: always reserved, so a validation warning or the FFmpeg note never changes the dialog height.
        {
            std::string note;
            char const* noteIcon = ICON_WARNING;
            ImU32       noteTone = theme::kWarning;
            if (!validSettings) {
                if (!validOutput) note = "playback.refactorEditor.export.invalidOutput"_tr();
                else if (!validTimeline) note = "playback.refactorEditor.export.invalidTimeline"_tr();
                else if (!validFps) note = "playback.refactorEditor.export.invalidFps"_tr();
                else if (!validResolution || !validCapture) note = "playback.refactorEditor.export.invalidCapture"_tr();
                else if (!formatAvailable) note = "playback.refactorEditor.export.ffmpegUnavailable"_tr();
                else note = "playback.refactorEditor.export.invalidSettings"_tr();
            } else if (!capabilities.ffmpegVideoExport) {
                note     = "playback.refactorEditor.export.ffmpegUnavailable"_tr();
                noteIcon = ICON_INFO;
                noteTone = theme::kTextDim;
            }
            float const  pad    = 10.0f * uiScale;
            float const  stripH = frameH + 6.0f * uiScale;
            float const  stripW = ImGui::GetContentRegionAvail().x;
            ImVec2 const min    = ImGui::GetCursorScreenPos();
            ImVec2 const max{min.x + stripW, min.y + stripH};
            ImGui::Dummy({stripW, stripH});
            if (!note.empty()) {
                auto* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(min, max, theme::withAlpha(noteTone, 0x22), theme::kFrameRounding);
                dl->AddRectFilled(min, {min.x + 3.0f * uiScale, max.y}, noteTone, theme::kFrameRounding);
                float const iconBox = metrics::iconGlyph();
                widgets::drawIconCentred(
                    dl,
                    noteIcon,
                    {min.x + pad, min.y + (stripH - iconBox) * 0.5f},
                    iconBox,
                    noteTone
                );
                ImVec4 const clip{min.x, min.y, max.x - pad, max.y};
                dl->AddText(
                    nullptr,
                    0.0f,
                    {min.x + pad + iconBox + pad * 0.75f, min.y + widgets::textOffsetInBox(stripH, note.c_str())},
                    theme::kText,
                    note.c_str(),
                    nullptr,
                    0.0f,
                    &clip
                );
                widgets::itemTooltip(!compiled.message.empty() ? compiled.message.c_str() : note.c_str());
            }
        }

        ImGui::PopStyleVar();
        ImGui::Separator();
        ImGui::Dummy({0.0f, 4.0f * uiScale});

        // Footer: quiet cancel on the left of an accent-filled primary action, both one button height tall.
        std::string const cancelLabel = "playback.refactorEditor.export.cancel"_tr();
        std::string const startLabel  = std::string(ICON_EXPORT) + "  " + "playback.refactorEditor.export.export"_tr();
        float const       labelPad    = style.FramePadding.x * 4.0f;
        float const cancelWidth = std::max(96.0f * uiScale, ImGui::CalcTextSize(cancelLabel.c_str()).x + labelPad);
        float const startWidth  = std::max(150.0f * uiScale, ImGui::CalcTextSize(startLabel.c_str()).x + labelPad);
        float const footerWidth = cancelWidth + style.ItemSpacing.x + startWidth;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - footerWidth - dialogPadding.x));
        ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::kButton, 0x00));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
        if (ImGui::Button(cancelLabel.c_str(), {cancelWidth, buttonH})) {
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::BeginDisabled(!validSettings || exportActive);
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kIconHighlight);
        if (ImGui::Button(startLabel.c_str(), {startWidth, buttonH})) {
            EditorAction action{EditorActionType::StartExport};
            action.exportSettings = std::move(previewSettings);
            ctx.submitAction(std::move(action));
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(4);
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
