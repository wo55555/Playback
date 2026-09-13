#include "EditorTheme.h"

#include "imgui.h"

namespace playback::editor::ui::theme {

void apply() {
    auto& style = ImGui::GetStyle();

    // Spacing follows the font so raising the UI scale grows padding along with glyphs; borders stay 1px.
    float const scale = metrics::scale();

    style.WindowPadding     = {kPanelPadding * scale, kPanelPadding * scale};
    style.FramePadding      = {(kItemSpacing + 1.0f) * scale, kItemSpacing * scale};
    style.ItemSpacing       = {kItemSpacing * scale, kItemSpacing * scale};
    style.ItemInnerSpacing  = {kItemSpacing * scale, kItemSpacing * scale};
    style.FrameRounding     = kFrameRounding;
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 0.0f;
    style.GrabRounding      = kFrameRounding;
    style.PopupRounding     = kFrameRounding;
    style.ScrollbarSize     = 10.0f * scale;
    style.ScrollbarRounding = 2.0f;
    style.WindowBorderSize  = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;

    auto* colors                          = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImColor(static_cast<int>(kBgPanel));
    colors[ImGuiCol_ChildBg]              = ImColor(0, 0, 0, 0);
    colors[ImGuiCol_PopupBg]              = ImColor(static_cast<int>(kBgHeader));
    colors[ImGuiCol_MenuBarBg]            = ImColor(static_cast<int>(kBgHeader));
    colors[ImGuiCol_Border]               = ImColor(static_cast<int>(kBorder));
    colors[ImGuiCol_BorderShadow]         = ImColor(0, 0, 0, 0);
    colors[ImGuiCol_Text]                 = ImColor(static_cast<int>(kText));
    colors[ImGuiCol_TextDisabled]         = ImColor(static_cast<int>(kTextDim));
    colors[ImGuiCol_Button]               = ImColor(static_cast<int>(kButton));
    colors[ImGuiCol_ButtonHovered]        = ImColor(static_cast<int>(kButtonHover));
    colors[ImGuiCol_ButtonActive]         = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_Header]               = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_HeaderHovered]        = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_HeaderActive]         = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_FrameBg]              = ImColor(static_cast<int>(kInputBg));
    colors[ImGuiCol_FrameBgHovered]       = ImColor(static_cast<int>(kInputBgHover));
    colors[ImGuiCol_FrameBgActive]        = ImColor(static_cast<int>(kInputBgActive));
    colors[ImGuiCol_TitleBg]              = ImColor(static_cast<int>(kBgHeader));
    colors[ImGuiCol_TitleBgActive]        = ImColor(static_cast<int>(kBgHeader));
    colors[ImGuiCol_TitleBgCollapsed]     = ImColor(static_cast<int>(kBgPanel));
    colors[ImGuiCol_CheckMark]            = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_SliderGrab]           = ImColor(static_cast<int>(kScrollThumb));
    colors[ImGuiCol_SliderGrabActive]     = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_ScrollbarBg]          = ImColor(static_cast<int>(kBgPanel));
    colors[ImGuiCol_ScrollbarGrab]        = ImColor(static_cast<int>(kScrollThumb));
    colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(static_cast<int>(kButtonHover));
    colors[ImGuiCol_ScrollbarGrabActive]  = ImColor(static_cast<int>(kButtonActive));
    colors[ImGuiCol_Separator]            = ImColor(static_cast<int>(kBorder));
    colors[ImGuiCol_TableBorderLight]     = ImColor(static_cast<int>(kBorder));
    colors[ImGuiCol_TableBorderStrong]    = ImColor(static_cast<int>(kBorder));
    colors[ImGuiCol_ResizeGrip]           = ImColor(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered]    = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_ResizeGripActive]     = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_ModalWindowDimBg]     = ImColor(static_cast<int>(kModalDim));
}

} // namespace playback::editor::ui::theme
