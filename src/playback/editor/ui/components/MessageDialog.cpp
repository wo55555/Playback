#include "MessageDialog.h"

#include "playback/editor/ui/EditorTheme.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

namespace playback::editor::ui {

namespace {

constexpr ImU32 kDialogBg      = IM_COL32(0x20, 0x21, 0x24, 0xff);
constexpr ImU32 kDialogBorder  = IM_COL32(0x4c, 0x50, 0x56, 0xdc);
constexpr ImU32 kDialogText    = IM_COL32(0xee, 0xf0, 0xf4, 0xff);
constexpr ImU32 kDialogTextDim = IM_COL32(0xa4, 0xa8, 0xb0, 0xff);

struct ButtonColors {
    ImU32 idle;
    ImU32 hover;
    ImU32 active;
};

ButtonColors buttonColors(DialogButtonKind kind) {
    switch (kind) {
    case DialogButtonKind::Primary:
        return {theme::kAccent, theme::kAccentHover, theme::kAccent};
    case DialogButtonKind::Danger:
        return {IM_COL32(0xc8, 0x3c, 0x3c, 0xff), IM_COL32(0xdc, 0x52, 0x52, 0xff), IM_COL32(0xb0, 0x34, 0x34, 0xff)};
    case DialogButtonKind::Secondary:
    default:
        return {IM_COL32(0x34, 0x36, 0x3a, 0xff), IM_COL32(0x42, 0x45, 0x4a, 0xff), IM_COL32(0x4e, 0x51, 0x58, 0xff)};
    }
}

// Body text lines up with the title rather than the badge.
float gIndent{};
// Measured by the previous frame; fractional heights otherwise raise a scrollbar on an auto-fit window.
bool  gScrolls{};
float gMaxHeight{};
float gPadding{};

} // namespace

bool beginMessageDialog(char const* id, char const* icon, ImU32 tone, std::string_view title, float fontScale) {
    // The popup's own font size, which the caller's window scale does not affect.
    ImGuiStyle const&    style    = ImGui::GetStyle();
    float const          font     = style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi * fontScale;
    ImGuiViewport const* viewport = ImGui::GetMainViewport();
    float const          width    = std::max(font * 8.0f, std::min(font * 24.0f, viewport->WorkSize.x - font * 2.0f));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    gMaxHeight = std::max(font * 8.0f, viewport->WorkSize.y - font * 2.0f);
    gPadding   = font * 1.2f;
    ImGui::SetNextWindowSizeConstraints({width, 0.0f}, {width, gMaxHeight});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {font * 1.4f, font * 1.2f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, font * 0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kDialogBg);
    ImGui::PushStyleColor(ImGuiCol_Border, kDialogBorder);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!gScrolls) flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool const open = ImGui::BeginPopupModal(id, nullptr, flags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (!open) return false;

    ImGui::SetWindowFontScale(fontScale);
    float const lineFont = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {lineFont * 0.5f, lineFont * 0.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, lineFont * 0.35f);

    float const  badge  = lineFont * 2.0f;
    float const  gap    = lineFont * 0.8f;
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    ImVec2 const centre{origin.x + badge * 0.5f, origin.y + badge * 0.5f};
    ImDrawList*  draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(centre, badge * 0.5f, theme::withAlpha(tone, 0x30));
    ImVec2 const iconSize = ImGui::CalcTextSize(icon);
    draw->AddText({centre.x - iconSize.x * 0.5f, centre.y - iconSize.y * 0.5f}, tone, icon);

    ImGui::SetCursorScreenPos({origin.x + badge + gap, origin.y + (badge - ImGui::GetTextLineHeight()) * 0.5f});
    ImGui::PushStyleColor(ImGuiCol_Text, kDialogText);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(title.data(), title.data() + title.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos({origin.x, std::max(ImGui::GetCursorScreenPos().y, origin.y + badge)});

    gIndent = badge + gap;
    ImGui::Indent(gIndent);
    return true;
}

void dialogText(std::string_view text, bool secondary) {
    ImGui::PushStyleColor(ImGuiCol_Text, secondary ? kDialogTextDim : kDialogText);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

int dialogButtons(std::initializer_list<DialogButton> buttons) {
    float const font    = ImGui::GetFontSize();
    float const height  = font * 2.1f;
    float const padding = font * 1.2f;
    float const gap     = font * 0.5f;

    std::vector<std::string> labels;
    std::vector<float>       widths;
    float                    total{};
    int                      confirm = -1;
    int                      cancel  = -1;
    for (auto const& button : buttons) {
        auto const index = static_cast<int>(labels.size());
        if (button.kind == DialogButtonKind::Primary && !button.disabled && confirm < 0) confirm = index;
        if (button.kind == DialogButtonKind::Secondary && cancel < 0) cancel = index;
        labels.emplace_back(button.label);
        widths.push_back(std::max(font * 5.5f, ImGui::CalcTextSize(labels.back().c_str()).x + padding * 2.0f));
        total += widths.back() + (index > 0 ? gap : 0.0f);
    }
    if (cancel < 0 && buttons.size() == 1) cancel = 0;

    ImGui::Unindent(gIndent);
    ImGui::Dummy({0.0f, font * 0.3f});
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - total));

    int clicked = -1;
    int index   = 0;
    for (auto const& button : buttons) {
        if (index > 0) ImGui::SameLine(0.0f, gap);
        auto const colors = buttonColors(button.kind);
        ImGui::PushStyleColor(ImGuiCol_Button, colors.idle);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.active);
        ImGui::PushStyleColor(ImGuiCol_Text, kDialogText);
        ImGui::PushID(index);
        ImGui::BeginDisabled(button.disabled);
        if (ImGui::Button(labels[static_cast<size_t>(index)].c_str(), {widths[static_cast<size_t>(index)], height})) {
            clicked = index;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::PopStyleColor(4);
        ++index;
    }
    ImGui::Indent(gIndent);

    if (clicked < 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        bool const enter =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (enter && confirm >= 0) clicked = confirm;
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && cancel >= 0) clicked = cancel;
    }
    return clicked;
}

void endMessageDialog() {
    gScrolls = ImGui::GetCursorPosY() + gPadding > gMaxHeight + 1.0f;
    ImGui::Unindent(gIndent);
    ImGui::PopStyleVar(2);
    ImGui::EndPopup();
}

} // namespace playback::editor::ui
