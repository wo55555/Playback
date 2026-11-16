#include "EditorMenuBar.h"

#include "playback/Playback.h"
#include "playback/editor/input/KeyMap.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/exporting/ExportPlanCompiler.h"
#include "playback/utils/ShellUtils.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

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

// Typed digits apply as they are entered, so leaving the field any way (the export button included) keeps them.
// The value is clamped on every edit; the field shows the clamped number once it loses focus.
bool inputClampedInt(char const* id, int& value, int minimum, int maximum, float width) {
    ImGui::SetNextItemWidth(width);
    int        edited = value;
    bool const changed =
        ImGui::InputInt(id, &edited, 1, 10, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
    if (changed) value = std::clamp(edited, minimum, maximum);
    return changed;
}

// Dim caption with a hairline underneath, used for every group in the export dialog.
void sectionHeader(char const* label, float top = 10.0f) {
    ImGui::Dummy({0.0f, top * metrics::scale()});
    ImGui::TextDisabled("%s", label);
    ImVec2 const min   = ImGui::GetItemRectMin();
    float const  y     = ImGui::GetItemRectMax().y + 3.0f * metrics::scale();
    float const  right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine({min.x, y}, {right, y}, theme::kBorder);
    ImGui::Dummy({0.0f, 1.0f * metrics::scale()});
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

// Row of page tabs: plain labels over a full-width hairline, the current page underlined in the accent colour.
bool tabStrip(char const* id, char const* const* labels, int count, int& index) {
    float const  s      = metrics::scale();
    float const  height = ImGui::GetFrameHeight() + 4.0f * s;
    float const  padX   = 10.0f * s;
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const  right  = origin.x + ImGui::GetContentRegionAvail().x;
    auto*        dl     = ImGui::GetWindowDrawList();
    dl->AddLine({origin.x, origin.y + height}, {right, origin.y + height}, theme::kBorder);

    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, ImGui::GetStyle().ItemSpacing.y});
    // The first label lines up with the content below; its hit area extends into the window padding instead.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - padX);
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine();
        ImVec2 const textSize = ImGui::CalcTextSize(labels[i]);
        ImVec2 const min      = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", {textSize.x + padX * 2.0f, height}) && i != index) {
            index   = i;
            changed = true;
        }
        ImGui::PopID();
        bool const active  = i == index;
        bool const hovered = ImGui::IsItemHovered();
        if (hovered && !active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImU32 const tone = active ? theme::kIconHighlight : hovered ? theme::kText : theme::kTextDim;
        dl->AddText({min.x + padX, min.y + (height - textSize.y) * 0.5f}, tone, labels[i]);
        if (active) {
            dl->AddRectFilled(
                {min.x + padX, min.y + height - 2.0f * s},
                {min.x + padX + textSize.x, min.y + height + 1.0f},
                theme::kAccent
            );
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
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

constexpr exporting::AudioCodec kAudioCodecs[] = {
    exporting::AudioCodec::Aac,
    exporting::AudioCodec::Opus,
    exporting::AudioCodec::Flac,
};
constexpr uint32_t kAudioBitratesKbps[] = {128, 192, 256, 320};

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
    // Only wraps once the dialog is narrower than its text, i.e. on a viewport too narrow for the sheet.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(description.c_str());
    ImGui::PopTextWrapPos();
}

std::string shortcutPair(input::EditorKeybind first, input::EditorKeybind second) {
    return input::KeyMap::displayString(first) + " / " + input::KeyMap::displayString(second);
}

struct ShortcutGroup {
    std::string                                      title;
    std::vector<std::pair<std::string, std::string>> rows; // keys, description
};

[[nodiscard]] std::vector<ShortcutGroup> shortcutGroups() {
    using input::EditorKeybind;
    auto const key = [](EditorKeybind bind) { return input::KeyMap::displayString(bind); };
    return {
        {"playback.refactorEditor.shortcuts.playback"_tr(),
         {
             {key(EditorKeybind::TogglePlayback), "playback.refactorEditor.shortcuts.playPause"_tr()},
             {shortcutPair(EditorKeybind::JumpStart, EditorKeybind::JumpEnd),
              "playback.refactorEditor.shortcuts.jumpEdges"_tr()},
             {shortcutPair(EditorKeybind::SeekSecondLeft, EditorKeybind::SeekSecondRight),
              "playback.refactorEditor.shortcuts.seekSeconds"_tr()},
             {shortcutPair(EditorKeybind::SeekTickLeft, EditorKeybind::SeekTickRight),
              "playback.refactorEditor.shortcuts.seekTicks"_tr()},
             {shortcutPair(EditorKeybind::PreviousEditPoint, EditorKeybind::NextEditPoint),
              "playback.refactorEditor.shortcuts.editPoints"_tr()},
             {shortcutPair(EditorKeybind::DecreaseSpeed, EditorKeybind::IncreaseSpeed),
              "playback.refactorEditor.shortcuts.speed"_tr()},
             {"Shift", "playback.refactorEditor.shortcuts.stopFollow"_tr()},
             {"F5", "playback.refactorEditor.shortcuts.togglePerspective"_tr()},
         }},
        {"playback.refactorEditor.shortcuts.editing"_tr(),
         {
             {shortcutPair(EditorKeybind::Undo, EditorKeybind::Redo),
              "playback.refactorEditor.shortcuts.undoRedo"_tr()},
             {key(EditorKeybind::SaveProject), "playback.refactorEditor.shortcuts.saveProject"_tr()},
             {key(EditorKeybind::AddKeyframe), "playback.refactorEditor.shortcuts.keyframe"_tr()},
             {key(EditorKeybind::AddCameraTrack), "playback.refactorEditor.shortcuts.addCamera"_tr()},
             {"1 / 2 / 3", "playback.refactorEditor.shortcuts.selectCamera"_tr()},
             {key(EditorKeybind::DeleteSelection), "playback.refactorEditor.shortcuts.delete"_tr()},
             {key(EditorKeybind::SelectAll), "playback.refactorEditor.shortcuts.selectAll"_tr()},
             {"Ctrl+Click / Drag", "playback.refactorEditor.shortcuts.boxSelect"_tr()},
             {key(EditorKeybind::ClearSelection), "playback.refactorEditor.shortcuts.clearSelection"_tr()},
             {shortcutPair(EditorKeybind::Copy, EditorKeybind::Paste),
              "playback.refactorEditor.shortcuts.copyPaste"_tr()},
             {"Shift+Drag", "playback.refactorEditor.shortcuts.shiftSnap"_tr()},
             {key(EditorKeybind::InsertMarker), "playback.refactorEditor.shortcuts.insertMarker"_tr()},
             {shortcutPair(EditorKeybind::PreviousMarker, EditorKeybind::NextMarker),
              "playback.refactorEditor.shortcuts.markers"_tr()},
             {shortcutPair(EditorKeybind::ZoomOutTimeline, EditorKeybind::ZoomInTimeline),
              "playback.refactorEditor.shortcuts.zoom"_tr()},
             {"Alt+Wheel", "playback.refactorEditor.shortcuts.zoomWheel"_tr()},
             {key(EditorKeybind::ResetTimelineZoom), "playback.refactorEditor.shortcuts.zoomReset"_tr()},
             {shortcutPair(EditorKeybind::MarkExportIn, EditorKeybind::MarkExportOut),
              "playback.refactorEditor.shortcuts.exportRange"_tr()},
         }},
        {"playback.refactorEditor.shortcuts.application"_tr(),
         {
             {key(EditorKeybind::OpenExport), "playback.refactorEditor.shortcuts.export"_tr()},
             {key(EditorKeybind::ToggleViewportMaximized), "playback.refactorEditor.shortcuts.viewport"_tr()},
             {shortcutPair(EditorKeybind::DecreaseUiScale, EditorKeybind::IncreaseUiScale),
              "playback.refactorEditor.shortcuts.uiScale"_tr()},
             {key(EditorKeybind::ResetUiScale), "playback.refactorEditor.shortcuts.uiScaleReset"_tr()},
         }},
    };
}

// One column of the shortcut sheet. Its groups share a key column, so every description starts on the same edge.
struct ShortcutColumn {
    std::vector<size_t> groups; // indices into shortcutGroups()
    float               keyWidth{};
    float               width{};
    float               height{};
};

// Font size of a popup opened from the menu bar: the bar's own window runs a size up, and popups do not inherit that.
[[nodiscard]] float popupFontSize() {
    ImGuiStyle const& style = ImGui::GetStyle();
    // ImGui rounds the final size to whole pixels; measuring at the unrounded size comes out a few pixels short.
    return std::max(1.0f, std::floor(style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi + 0.5f));
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
        auto const stamp = timestampName();
        std::snprintf(mExportName.data(), mExportName.size(), "%s", stamp.c_str());
    }
    mExportSettingsInitialized = true;
    if (!ffmpegAvailable && mExportFormat == 0) mExportFormat = 1;
    // Always opens on the first page; the page height is measured again in case the font changed meanwhile.
    mExportTab            = 0;
    mExportBodyHeight     = 0.0f;
    mExportBodyScrollable = false;
    mExportDialogOpen     = true;
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
        if (widgets::beginMenu("playback.refactorEditor.menu.file"_tr().c_str())) {
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
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.exit"_tr().c_str(),
                    "playback.refactorEditor.menu.exitShortcut"_tr().c_str()
                )) {
                ctx.submitAction({playback::state::EditorActionType::StopReplay});
            }
            widgets::endMenu();
        }

        if (widgets::beginMenu("playback.refactorEditor.menu.edit"_tr().c_str())) {
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
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.selectAll"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::SelectAll).c_str(),
                    false,
                    state.project != nullptr
                )) {
                (void)ctx.commands.selectAllKeyframes();
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.copy"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::Copy).c_str(),
                    false,
                    ctx.selection.keyframeCount() > 0
                )) {
                (void)ctx.commands.copySelection();
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.menu.paste"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::Paste).c_str(),
                    false,
                    state.project != nullptr
                )) {
                (void)ctx.commands.pasteAtPlayhead();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.addMarker"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::InsertMarker).c_str(),
                    false,
                    state.project != nullptr
                )) {
                (void)ctx.commands.addMarkerAtPlayhead();
            }
            widgets::endMenu();
        }

        if (widgets::beginMenu("playback.refactorEditor.menu.view"_tr().c_str())) {
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
                    "playback.refactorEditor.menu.maximizeViewport"_tr().c_str(),
                    input::KeyMap::displayString(input::EditorKeybind::ToggleViewportMaximized).c_str(),
                    ctx.commands.isViewportMaximized(),
                    viewEnabled
                )) {
                ctx.commands.toggleViewportMaximized();
            }
            widgets::endMenu();
        }

        if (widgets::beginMenu("playback.refactorEditor.menu.help"_tr().c_str())) {
            if (ImGui::MenuItem("playback.refactorEditor.menu.shortcuts"_tr().c_str())) mShortcutDialogOpen = true;
            widgets::endMenu();
        }

        ImGui::EndMenuBar();
    }
}

void EditorMenuBar::drawShortcutDialog() {
    char const* const popupId = "##KeyboardShortcuts";
    if (mShortcutDialogOpen) ImGui::OpenPopup(popupId);
    if (!ImGui::IsPopupOpen(popupId)) return;

    // Measured at the popup's own font size, so the sheet is sized and laid out before it opens.
    ImGuiStyle const& style     = ImGui::GetStyle();
    float const       font      = popupFontSize();
    float const       scale     = std::max(1.0f, font / 14.0f);
    ImFont* const     fontFace  = ImGui::GetFont();
    auto const        textWidth = [&](std::string const& text) {
        return std::ceil(fontFace->CalcTextSizeA(font, FLT_MAX, 0.0f, text.c_str()).x);
    };
    ImVec2 const padding{20.0f * scale, 14.0f * scale};
    ImVec2 const cellPadding{8.0f * scale, 2.0f * scale};
    float const  rowHeight    = font + cellPadding.y * 2.0f;
    float const  buttonHeight = font + style.FramePadding.y * 2.0f + 6.0f * scale;
    float const  columnGap    = 32.0f * scale;
    float const  headerTop    = 8.0f;

    std::vector<ShortcutGroup> const groups      = shortcutGroups();
    auto const                       buildColumn = [&](std::initializer_list<size_t> indices) {
        ShortcutColumn column;
        float          descriptionWidth = 0.0f;
        for (size_t const index : indices) {
            auto const& group = groups[index];
            column.groups.push_back(index);
            // Mirrors sectionHeader plus one table row per shortcut.
            column.height += (headerTop + 1.0f) * scale + font + style.ItemSpacing.y * 4.0f
                           + rowHeight * static_cast<float>(group.rows.size());
            column.width = std::max(column.width, textWidth(group.title));
            for (auto const& [keys, description] : group.rows) {
                column.keyWidth  = std::max(column.keyWidth, textWidth(keys));
                descriptionWidth = std::max(descriptionWidth, textWidth(description));
            }
        }
        // The slack keeps the longest description from wrapping over a rounding difference.
        column.width = std::max(column.width, column.keyWidth + cellPadding.x * 2.0f + descriptionWidth + 2.0f * scale);
        return column;
    };
    // Playback and application shortcuts together are about as tall as the editing list.
    ShortcutColumn const stacked = buildColumn({0, 1, 2});
    ShortcutColumn const left    = buildColumn({0, 2});
    ShortcutColumn const right   = buildColumn({1});

    auto const* const viewport  = ImGui::GetMainViewport();
    float const       margin    = 12.0f * scale;
    float const       maxWidth  = std::max(1.0f, viewport->WorkSize.x - margin * 2.0f);
    float const       maxHeight = std::max(1.0f, viewport->WorkSize.y - margin * 2.0f);
    // Title bar, title line, both separators and the close button around the scrolling list.
    float const chromeHeight =
        font + style.FramePadding.y * 2.0f + padding.y * 2.0f + font + buttonHeight + style.ItemSpacing.y * 4.0f + 2.0f;
    // The list keeps a scrollbar's width spare, so one appearing never squeezes the text.
    float const frameWidth     = padding.x * 2.0f + style.ScrollbarSize;
    float const twoColumnWidth = frameWidth + left.width + columnGap + right.width;
    // Two columns once one would not fit the viewport's height, as long as the viewport is wide enough for them.
    bool const  twoColumns  = chromeHeight + stacked.height > maxHeight && twoColumnWidth <= maxWidth;
    float const dialogWidth = std::min(maxWidth, twoColumns ? twoColumnWidth : frameWidth + stacked.width);

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize({dialogWidth, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({dialogWidth, 0.0f}, {dialogWidth, maxHeight});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    bool const visible = ImGui::BeginPopupModal(
        popupId,
        &mShortcutDialogOpen,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse
    );
    ImGui::PopStyleVar(2);
    if (!visible) return;

    ImGui::TextUnformatted("playback.refactorEditor.shortcuts.title"_tr().c_str());
    ImGui::Separator();

    // Only the list scrolls on a short viewport; the close button below it always stays on screen.
    float const footerHeight  = style.ItemSpacing.y * 2.0f + 1.0f + buttonHeight + padding.y;
    float const maxBodyHeight = std::max(rowHeight * 4.0f, maxHeight - ImGui::GetCursorPosY() - footerHeight);
    ImGui::SetNextWindowSizeConstraints({0.0f, 0.0f}, {FLT_MAX, maxBodyHeight});
    ImGui::BeginChild("##shortcut-body", {0.0f, 0.0f}, ImGuiChildFlags_AutoResizeY);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);
    auto const drawColumn = [&](ShortcutColumn const& column) {
        for (size_t const index : column.groups) {
            auto const& group = groups[index];
            ImGui::PushID(static_cast<int>(index));
            sectionHeader(group.title.c_str(), headerTop);
            if (ImGui::BeginTable("##shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed, column.keyWidth);
                ImGui::TableSetupColumn("##command", ImGuiTableColumnFlags_WidthStretch);
                for (auto const& [keys, description] : group.rows) shortcutRow(keys, description);
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    };
    if (twoColumns) {
        // Spacing between the two columns is twice the horizontal cell padding.
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {columnGap * 0.5f, 0.0f});
        bool const columns = ImGui::BeginTable("##shortcut-columns", 2, ImGuiTableFlags_SizingStretchProp);
        if (columns) {
            ImGui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthFixed, left.width);
            ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
        }
        ImGui::PopStyleVar();
        if (columns) {
            ImGui::TableSetColumnIndex(0);
            drawColumn(left);
            ImGui::TableSetColumnIndex(1);
            drawColumn(right);
            ImGui::EndTable();
        }
    } else {
        drawColumn(stacked);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::Separator();
    std::string const closeLabel = "playback.refactorEditor.shortcuts.close"_tr();
    float const       closeWidth =
        std::max(96.0f * scale, ImGui::CalcTextSize(closeLabel.c_str()).x + style.FramePadding.x * 4.0f);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - closeWidth - padding.x));
    if (ImGui::Button(closeLabel.c_str(), {closeWidth, buttonHeight})) {
        mShortcutDialogOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorMenuBar::drawExportDialog(PanelContext const& ctx) {
    auto const& state        = ctx.state;
    auto const& capabilities = state.capabilities;
    auto const  exportActive = exporting::isExportActive(state.exportStatus.state);

    if (mExportDialogOpen) ImGui::OpenPopup("##ExportVideo");
    if (!ImGui::IsPopupOpen("##ExportVideo")) return;

    ImGuiStyle const& style          = ImGui::GetStyle();
    ImVec2 const      exportWorkSize = ImGui::GetMainViewport()->WorkSize;
    float const       uiScale        = metrics::scale();
    ImVec2 const      exportDialogSize{std::max(1.0f, std::min(580.0f * uiScale, exportWorkSize.x - 24.0f)), 0.0f};
    ImVec2 const      dialogPadding{22.0f * uiScale, 16.0f * uiScale};
    float const       insetBleed      = 8.0f * uiScale;
    float const       cellPaddingY    = 2.0f * uiScale;
    float const       fieldWidth      = 92.0f * uiScale;
    float const       maxDialogHeight = std::max(1.0f, exportWorkSize.y - 24.0f);

    // The popup draws at its own font size rather than the menu bar's, so the layout is measured at that size.
    float const   font      = popupFontSize();
    float const   frameH    = font + style.FramePadding.y * 2.0f;
    float const   buttonH   = frameH + 6.0f * uiScale;
    ImFont* const fontFace  = ImGui::GetFont();
    auto const    textWidth = [&](std::string const& text) {
        return std::ceil(fontFace->CalcTextSizeA(font, FLT_MAX, 0.0f, text.c_str()).x);
    };

    // One label column for every page; it must fit the longest label that carries a "(?)" hint.
    float const hintExtra        = style.ItemInnerSpacing.x + textWidth("(?)") + style.CellPadding.x * 2.0f;
    float       hintedLabelWidth = 0.0f;
    for (std::string const& label :
         {"playback.refactorEditor.export.ssaa"_tr(),
          "playback.refactorEditor.export.warmupFrames"_tr(),
          "playback.refactorEditor.export.hideHud"_tr(),
          "playback.refactorEditor.export.encodeSpeed"_tr(),
          "playback.refactorEditor.export.crf"_tr(),
          "playback.refactorEditor.export.recordAudio"_tr(),
          "playback.refactorEditor.export.audioCodec"_tr(),
          "playback.refactorEditor.export.audioBitrate"_tr(),
          "playback.refactorEditor.export.extraRenders"_tr()}) {
        hintedLabelWidth = std::max(hintedLabelWidth, textWidth(label) + hintExtra);
    }
    // Fixed label column aligns every value; the capped value column stops inputs spanning the dialog.
    float const labelWidth = std::min(
        std::max(std::clamp(exportDialogSize.x * 0.26f, 96.0f * uiScale, 150.0f * uiScale), hintedLabelWidth),
        exportDialogSize.x * 0.4f
    );
    float const valueWidth = std::min(
        320.0f * uiScale,
        exportDialogSize.x - dialogPadding.x * 2.0f - labelWidth - style.CellPadding.x * 4.0f
    );
    // Width/height row: two fields plus the "x" between them; preset rows end on the same edge.
    float const resolutionRowWidth = fieldWidth * 2.0f + textWidth("x") + style.ItemSpacing.x * 2.0f;

    // Drawn below the page: gap and summary block, note strip, separator, gap and footer.
    constexpr int kSummaryRows = 4;
    float const   summaryRowH  = font + cellPaddingY * 2.0f;
    float const belowBody = 6.0f * uiScale + (12.0f * uiScale + summaryRowH * kSummaryRows) + (frameH + 6.0f * uiScale)
                          + 1.0f + 5.0f * uiScale + buttonH + style.ItemSpacing.y * 6.0f + dialogPadding.y;
    // The shared page height was measured at one font size.
    if (font != mExportLayoutKey) {
        mExportLayoutKey  = font;
        mExportBodyHeight = 0.0f;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Height fits the content; on a viewport shorter than that the page scrolls and the footer stays in place.
    ImGui::SetNextWindowSize(exportDialogSize, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints({exportDialogSize.x, 0.0f}, {exportDialogSize.x, maxDialogHeight});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, dialogPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * uiScale);
    // The page child's scrollbar slot overhangs the content by one scrollbar width, so the dialog's own bars are
    // hidden.
    bool const dialogVisible = ImGui::BeginPopupModal(
        "##ExportVideo",
        &mExportDialogOpen,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
    );
    ImGui::PopStyleVar(2);
    if (dialogVisible) {
        std::string const custom                   = "playback.refactorEditor.export.custom"_tr();
        char const*       fpsOptions[]             = {"30 FPS", "60 FPS", "120 FPS", custom.c_str()};
        char const*       fpsShortOptions[]        = {"30", "60", "120", custom.c_str()};
        constexpr int     fpsValues[]              = {30, 60, 120};
        char const*       ssaaOptions[]            = {"1x", "2x", "4x"};
        char const*       resolutionPresetLabels[] = {"720p", "1080p", "1440p", "4K"};
        static_assert(IM_ARRAYSIZE(resolutionPresetLabels) == IM_ARRAYSIZE(kResolutionPresets));
        std::string const mp4Format             = "playback.refactorEditor.export.mp4"_tr();
        std::string const pngFormat             = "playback.refactorEditor.export.pngSequence"_tr();
        char const*       formatOptions[]       = {mp4Format.c_str(), pngFormat.c_str()};
        std::string const encodeQuality         = "playback.refactorEditor.export.encodeQuality"_tr();
        std::string const encodeFast            = "playback.refactorEditor.export.encodeFast"_tr();
        std::string const encodeFastest         = "playback.refactorEditor.export.encodeFastest"_tr();
        char const*       encodeSpeedOptions[]  = {encodeQuality.c_str(), encodeFast.c_str(), encodeFastest.c_str()};
        std::string const audioMono             = "playback.refactorEditor.export.audioMono"_tr();
        std::string const audioStereo           = "playback.refactorEditor.export.audioStereo"_tr();
        char const*       audioChannelOptions[] = {audioMono.c_str(), audioStereo.c_str()};
        char const*       audioCodecOptions[]   = {"AAC", "Opus", "FLAC"};
        static_assert(IM_ARRAYSIZE(audioCodecOptions) == IM_ARRAYSIZE(kAudioCodecs));
        char const* audioBitrateOptions[] = {"128k", "192k", "256k", "320k"};
        static_assert(IM_ARRAYSIZE(audioBitrateOptions) == IM_ARRAYSIZE(kAudioBitratesKbps));
        std::string const tabGeneral  = "playback.refactorEditor.export.tabGeneral"_tr();
        std::string const tabVideo    = "playback.refactorEditor.export.tabVideo"_tr();
        std::string const tabAudio    = "playback.refactorEditor.export.tabAudio"_tr();
        std::string const tabAdvanced = "playback.refactorEditor.export.tabAdvanced"_tr();
        char const*       tabLabels[] = {tabGeneral.c_str(), tabVideo.c_str(), tabAudio.c_str(), tabAdvanced.c_str()};
        enum ExportTab { TabGeneral, TabVideo, TabAudio, TabAdvanced };
        mExportTab                  = std::clamp(mExportTab, 0, IM_ARRAYSIZE(tabLabels) - 1);
        mExportAudioChannels        = std::clamp(mExportAudioChannels, 0, 1);
        mExportAudioCodec           = std::clamp(mExportAudioCodec, 0, IM_ARRAYSIZE(kAudioCodecs) - 1);
        mExportAudioBitrate         = std::clamp(mExportAudioBitrate, 0, IM_ARRAYSIZE(kAudioBitratesKbps) - 1);
        mExportEncodeSpeed          = std::clamp(mExportEncodeSpeed, 0, 2);
        auto const audioCodec       = kAudioCodecs[mExportAudioCodec];
        mExportSsaa                 = std::clamp(mExportSsaa, 0, 2);
        int const maximumReplayTick = std::max(state.totalTicks, 0);

        // Header: accent-tinted icon tile with title and subtitle stacked beside it.
        {
            float const  box    = metrics::iconButton();
            ImVec2 const origin = ImGui::GetCursorScreenPos();
            auto*        dl     = ImGui::GetWindowDrawList();
            dl->AddRectFilled(
                origin,
                {origin.x + box, origin.y + box},
                theme::withAlpha(theme::kAccent, 0x40),
                6.0f * uiScale
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

        // Settings are split over pages, so each one is short enough to show whole; the summary below spans them all.
        ImGui::Dummy({0.0f, 2.0f * uiScale});
        bool const tabChanged = tabStrip("##export-tabs", tabLabels, IM_ARRAYSIZE(tabLabels), mExportTab);

        // Taller cells give the rows room to breathe without widening the label column.
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {style.CellPadding.x, cellPaddingY});

        // Every page gets the height of the tallest one seen, so switching pages never resizes the dialog. On a
        // viewport too short for that, the read-only recap gives up its space first; only when that is not enough
        // does the page scroll, with the footer buttons still on screen.
        bool const  fixedBody       = mExportBodyHeight > 0.0f;
        float const roomWithSummary = maxDialogHeight - ImGui::GetCursorPosY() - belowBody;
        bool const  showSummary     = mExportBodyHeight <= roomWithSummary;
        float const summaryHeight   = 12.0f * uiScale + summaryRowH * kSummaryRows + style.ItemSpacing.y;
        float const maxBodyHeight   = std::max(frameH * 4.0f, roomWithSummary + (showSummary ? 0.0f : summaryHeight));
        // The scrollbar slot is always reserved (and invisible while nothing scrolls) so content width never shifts.
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, 0);
        if (!mExportBodyScrollable) {
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, 0);
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, 0);
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, 0);
        }
        ImGui::SetNextWindowSizeConstraints({0.0f, 0.0f}, {FLT_MAX, maxBodyHeight});
        ImGui::BeginChild(
            "##export-body",
            {ImGui::GetContentRegionAvail().x + style.ScrollbarSize,
             fixedBody ? std::min(mExportBodyHeight, maxBodyHeight) : 0.0f},
            fixedBody ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_AlwaysVerticalScrollbar
        );
        ImGui::PopStyleColor(mExportBodyScrollable ? 1 : 4);
        if (tabChanged) ImGui::SetScrollY(0.0f);
        // Known a frame late, which only delays the scrollbar grab appearing by that frame.
        mExportBodyScrollable = ImGui::GetScrollMaxY() > 0.0f;

        if (mExportTab == TabGeneral) {
            sectionHeader("playback.refactorEditor.export.output"_tr().c_str(), 6.0f);
            if (beginPropertyTable("##export-output", labelWidth)) {
                propertyLabel("playback.refactorEditor.export.name"_tr().c_str());
                ImGui::SetNextItemWidth(valueWidth);
                ImGui::InputText("##export-name", mExportName.data(), mExportName.size());

                propertyLabel("playback.refactorEditor.export.location"_tr().c_str());
                ImGui::SetNextItemWidth(valueWidth - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
                ImGui::InputText("##export-directory", mExportDirectory.data(), mExportDirectory.size());
                ImGui::SameLine();
                if (frameIconButton(
                        "##export-browse",
                        ICON_FOLDER_OPEN,
                        "playback.refactorEditor.export.browseLocation"_tr().c_str()
                    )) {
                    if (auto const picked = playback::utils::pickFolder(utf8Path(mExportDirectory.data()))) {
                        auto const text = picked->u8string();
                        std::snprintf(
                            mExportDirectory.data(),
                            mExportDirectory.size(),
                            "%s",
                            reinterpret_cast<char const*>(text.c_str())
                        );
                    }
                }

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
                // The unit is dropped once the buttons get too narrow for it; the row label already names it.
                float fpsLabelWidth = 0.0f;
                for (char const* option : fpsOptions)
                    fpsLabelWidth = std::max(fpsLabelWidth, ImGui::CalcTextSize(option).x);
                bool const fpsUnitFits =
                    presetWidth / IM_ARRAYSIZE(fpsOptions) >= fpsLabelWidth + style.FramePadding.x * 2.0f;
                if (segmented(
                        "##export-fps",
                        fpsUnitFits ? fpsOptions : fpsShortOptions,
                        IM_ARRAYSIZE(fpsOptions),
                        mFpsPreset,
                        presetWidth
                    )
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
        } else if (mExportTab == TabVideo) {
            sectionHeader("playback.refactorEditor.export.capture"_tr().c_str(), 6.0f);
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
                        resolutionRowWidth
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
                        resolutionRowWidth,
                        ssaaDisabledMask
                    ))
                    mExportSsaa = std::clamp(mExportSsaa, 0, 2);

                propertyLabel(
                    "playback.refactorEditor.export.warmupFrames"_tr().c_str(),
                    "playback.refactorEditor.export.warmupHint"_tr().c_str()
                );
                inputClampedInt("##export-warmup", mExportWarmupFrames, 0, 3600, fieldWidth);

                propertyLabel(
                    "playback.refactorEditor.export.hideHud"_tr().c_str(),
                    "playback.refactorEditor.export.hideHudHint"_tr().c_str()
                );
                ImGui::Checkbox("##export-hide-hud", &mExportHideHud);
                ImGui::EndTable();
            }

            sectionHeader("playback.refactorEditor.export.advancedEncode"_tr().c_str());
            if (beginPropertyTable("##export-encoding", labelWidth)) {
                // Labels stay live so their hints work; only the encoder widgets grey out for a PNG sequence.
                propertyLabel(
                    "playback.refactorEditor.export.encodeSpeed"_tr().c_str(),
                    "playback.refactorEditor.export.encodeSpeedHint"_tr().c_str()
                );
                ImGui::BeginDisabled(mExportFormat != 0);
                segmented(
                    "##export-encode-speed",
                    encodeSpeedOptions,
                    IM_ARRAYSIZE(encodeSpeedOptions),
                    mExportEncodeSpeed,
                    valueWidth
                );
                ImGui::EndDisabled();

                propertyLabel(
                    "playback.refactorEditor.export.crf"_tr().c_str(),
                    "playback.refactorEditor.export.crfHint"_tr().c_str()
                );
                ImGui::BeginDisabled(mExportFormat != 0);
                inputClampedInt("##export-crf", mExportCrf, 0, static_cast<int>(exporting::MaxExportCrf), fieldWidth);
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
        } else if (mExportTab == TabAudio) {
            sectionHeader("playback.refactorEditor.export.capture"_tr().c_str(), 6.0f);
            if (beginPropertyTable("##export-audio", labelWidth)) {
                propertyLabel(
                    "playback.refactorEditor.export.recordAudio"_tr().c_str(),
                    "playback.refactorEditor.export.recordAudioHint"_tr().c_str()
                );
                ImGui::Checkbox("##export-record-audio", &mExportAudio);

                // Rows stay in place while audio is off, so the page keeps its layout.
                propertyLabel("playback.refactorEditor.export.audioChannels"_tr().c_str());
                ImGui::BeginDisabled(!mExportAudio);
                segmented(
                    "##export-audio-channels",
                    audioChannelOptions,
                    IM_ARRAYSIZE(audioChannelOptions),
                    mExportAudioChannels,
                    resolutionRowWidth
                );
                ImGui::EndDisabled();
                ImGui::EndTable();
            }

            sectionHeader("playback.refactorEditor.export.advancedEncode"_tr().c_str());
            if (beginPropertyTable("##export-audio-encoding", labelWidth)) {
                // A PNG sequence keeps the captured sound as a WAV next to the frames, so there is nothing to encode.
                propertyLabel(
                    "playback.refactorEditor.export.audioCodec"_tr().c_str(),
                    "playback.refactorEditor.export.audioCodecHint"_tr().c_str()
                );
                ImGui::BeginDisabled(mExportFormat != 0 || !mExportAudio);
                segmented(
                    "##export-audio-codec",
                    audioCodecOptions,
                    IM_ARRAYSIZE(audioCodecOptions),
                    mExportAudioCodec,
                    resolutionRowWidth
                );
                ImGui::EndDisabled();

                propertyLabel(
                    "playback.refactorEditor.export.audioBitrate"_tr().c_str(),
                    "playback.refactorEditor.export.audioBitrateHint"_tr().c_str()
                );
                ImGui::BeginDisabled(
                    mExportFormat != 0 || !mExportAudio || !exporting::audioCodecUsesBitrate(audioCodec)
                );
                segmented(
                    "##export-audio-bitrate",
                    audioBitrateOptions,
                    IM_ARRAYSIZE(audioBitrateOptions),
                    mExportAudioBitrate,
                    valueWidth
                );
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
        } else {
            sectionHeader("playback.refactorEditor.export.advancedRender"_tr().c_str(), 6.0f);
            if (beginPropertyTable("##export-rendering", labelWidth)) {
                propertyLabel(
                    "playback.refactorEditor.export.extraRenders"_tr().c_str(),
                    "playback.refactorEditor.export.extraRendersHint"_tr().c_str()
                );
                inputClampedInt(
                    "##export-extra-renders",
                    mExportExtraRenders,
                    0,
                    static_cast<int>(exporting::MaxExportExtraRenders),
                    fieldWidth
                );
                ImGui::EndTable();
            }
        }

        // Rounded up: the child's size is truncated to whole pixels, and a fraction short would bring up a scrollbar.
        float const pageHeight = std::ceil(ImGui::GetCursorPosY() - style.ItemSpacing.y);
        ImGui::EndChild();
        mExportBodyHeight = std::max(mExportBodyHeight, pageHeight);

        int const      safeSsaaIndex = std::clamp(mExportSsaa, 0, 2);
        uint32_t const ssaaValue     = 1u << safeSsaaIndex;
        bool const     validOutput   = mExportName.front() != '\0' && mExportDirectory.front() != '\0';
        bool const     validTimeline =
            mExportStartTick >= 0 && mExportEndTick > mExportStartTick && mExportEndTick <= maximumReplayTick;
        bool const validFps        = mFps >= 1 && mFps <= 240;
        bool const validResolution = mExportWidth >= 16 && mExportHeight >= 16
                                  && static_cast<uint32_t>(mExportWidth) <= exporting::MaxExportResolution
                                  && static_cast<uint32_t>(mExportHeight) <= exporting::MaxExportResolution
                                  && static_cast<uint64_t>(mExportWidth) * mExportHeight <= exporting::MaxExportPixels
                                  && exporting::supersampleFits(
                                         static_cast<uint32_t>(mExportWidth),
                                         static_cast<uint32_t>(mExportHeight),
                                         ssaaValue
                                  );
        bool const validCapture = mExportSsaa >= 0 && mExportSsaa <= 2 && mExportWarmupFrames >= 0
                               && mExportWarmupFrames <= 3600 && mExportExtraRenders >= 0
                               && static_cast<uint32_t>(mExportExtraRenders) <= exporting::MaxExportExtraRenders
                               && mExportCrf >= 0 && static_cast<uint32_t>(mExportCrf) <= exporting::MaxExportCrf;
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
        previewSettings.extraRenders      = static_cast<uint32_t>(std::max(0, mExportExtraRenders));
        previewSettings.encodeSpeed = static_cast<exporting::VideoEncodeSpeed>(std::clamp(mExportEncodeSpeed, 0, 2));
        previewSettings.videoCrf          = static_cast<uint32_t>(std::clamp(mExportCrf, 0, 51));
        previewSettings.hideHud           = mExportHideHud;
        previewSettings.audio             = mExportAudio;
        previewSettings.audioStereo       = mExportAudioChannels == 1;
        previewSettings.audioCodec        = audioCodec;
        previewSettings.audioBitrateKbps  = kAudioBitratesKbps[mExportAudioBitrate];
        previewSettings.format =
            mExportFormat == 0 ? exporting::ExportFormat::Mp4Video : exporting::ExportFormat::PngSequence;

        exporting::ExportPlanCompileResult compiled;
        if (rawSettingsValid) compiled = exporting::ExportPlanCompiler::compile(previewSettings, *state.project);
        bool const     validSettings   = rawSettingsValid && static_cast<bool>(compiled);
        double const   durationSeconds = validTimeline ? (mExportEndTick - mExportStartTick) / 20.0 : 0.0;
        uint64_t const frameCount      = compiled.plan ? compiled.plan->frameCount : 0;

        // Read-only recap on a tinted block without an outline, so it reads apart from the editable rows above.
        ImGui::Dummy({0.0f, 6.0f * uiScale});
        if (showSummary) {
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
            // Encoder choices live on other pages than the one open, so the recap names them: video first, then audio.
            std::string encodeValue = mExportFormat == 0 ? std::string(encodeSpeedOptions[mExportEncodeSpeed])
                                                               + " | CRF " + std::to_string(mExportCrf)
                                                         : std::string("PNG");
            if (mExportAudio) {
                // A PNG sequence keeps its sound as a WAV file beside the frames.
                std::string audioValue = mExportFormat == 0 ? audioCodecOptions[mExportAudioCodec] : "WAV";
                if (mExportFormat == 0 && exporting::audioCodecUsesBitrate(audioCodec))
                    audioValue += std::string(" ") + audioBitrateOptions[mExportAudioBitrate];
                encodeValue += " | " + audioValue + " " + audioChannelOptions[mExportAudioChannels];
            } else {
                encodeValue += " | " + "playback.refactorEditor.export.audioSummaryOff"_tr();
            }
            // The block bleeds past the window padding while its rows stay on the shared label/value columns.
            float const  padY     = 6.0f * uiScale;
            float const  rowH     = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
            float const  blockH   = padY * 2.0f + rowH * kSummaryRows;
            float const  blockW   = ImGui::GetContentRegionAvail().x;
            ImVec2 const blockMin = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                {blockMin.x - insetBleed, blockMin.y},
                {blockMin.x + blockW + insetBleed, blockMin.y + blockH},
                theme::kBgPanel,
                6.0f * uiScale
            );
            ImGui::SetCursorScreenPos({blockMin.x, blockMin.y + padY});
            if (beginPropertyTable("##export-summary", labelWidth)) {
                auto const summaryRow = [](char const* label, std::string const& value) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%s", label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(value.c_str());
                };
                summaryRow("playback.refactorEditor.export.timelineSummary"_tr().c_str(), timelineValue);
                summaryRow("playback.refactorEditor.export.captureSummary"_tr().c_str(), captureValue);
                summaryRow("playback.refactorEditor.export.encodeSummary"_tr().c_str(), encodeValue);
                summaryRow("playback.refactorEditor.export.outputSummary"_tr().c_str(), outputValue);
                ImGui::EndTable();
            }
            ImGui::SetCursorScreenPos({blockMin.x, blockMin.y + blockH});
            ImGui::Dummy({blockW, 0.0f});
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
            ImVec2 const cursor = ImGui::GetCursorScreenPos();
            // Same bleed as the summary block, so both strips share their outer edges.
            ImVec2 const min{cursor.x - insetBleed, cursor.y};
            ImVec2 const max{cursor.x + stripW + insetBleed, cursor.y + stripH};
            ImGui::Dummy({stripW, stripH});
            if (!note.empty()) {
                auto* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(min, max, theme::withAlpha(noteTone, 0x22), 6.0f * uiScale);
                dl->AddRectFilled(
                    min,
                    {min.x + 3.0f * uiScale, max.y},
                    noteTone,
                    6.0f * uiScale,
                    ImDrawFlags_RoundCornersLeft
                );
                float const iconBox = metrics::iconGlyph();
                widgets::drawIconCentred(
                    dl,
                    noteIcon,
                    {cursor.x, min.y + (stripH - iconBox) * 0.5f},
                    iconBox,
                    noteTone
                );
                ImVec4 const clip{min.x, min.y, max.x - pad, max.y};
                dl->AddText(
                    nullptr,
                    0.0f,
                    {cursor.x + iconBox + pad * 0.75f, min.y + widgets::textOffsetInBox(stripH, note.c_str())},
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
        ImGui::Dummy({0.0f, 5.0f * uiScale});

        // Footer: quiet cancel on the left of an accent-filled primary action, both one button height tall.
        std::string const cancelLabel = "playback.refactorEditor.export.cancel"_tr();
        std::string const startLabel  = std::string(ICON_EXPORT) + "  " + "playback.refactorEditor.export.export"_tr();
        float const       labelPad    = style.FramePadding.x * 4.0f;
        float const cancelWidth = std::max(96.0f * uiScale, ImGui::CalcTextSize(cancelLabel.c_str()).x + labelPad);
        float const startWidth  = std::max(150.0f * uiScale, ImGui::CalcTextSize(startLabel.c_str()).x + labelPad);
        float const footerGap   = 8.0f * uiScale;
        float const footerWidth = cancelWidth + footerGap + startWidth;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - footerWidth - dialogPadding.x));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * uiScale);
        ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::kButton, 0x00));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
        auto const closeDialog = [&] {
            mExportDialogOpen = false;
            if (mExportStartTick <= 0 && mExportEndTick >= maximumReplayTick) {
                mExportStartTick = -1;
                mExportEndTick   = -1;
            }
            ImGui::CloseCurrentPopup();
        };
        if (ImGui::Button(cancelLabel.c_str(), {cancelWidth, buttonH})) {
            closeDialog();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0.0f, footerGap);
        ImGui::BeginDisabled(!validSettings || exportActive);
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kIconHighlight);
        if (ImGui::Button(startLabel.c_str(), {startWidth, buttonH})) {
            EditorAction action{EditorActionType::StartExport};
            action.exportSettings = std::move(previewSettings);
            ctx.submitAction(std::move(action));
            closeDialog();
        }
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        ImGui::PopStyleVar();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            closeDialog();
        }
        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
