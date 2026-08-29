#include "EditorTheme.h"

#include "imgui.h"

namespace playback::editor::ui::theme {

void apply() {
    auto& style = ImGui::GetStyle();

    style.WindowPadding     = {kPanelPadding, kPanelPadding};
    style.FramePadding      = {kItemSpacing, kItemSpacing};
    style.ItemSpacing       = {kItemSpacing, kItemSpacing};
    style.FrameRounding     = kFrameRounding;
    style.WindowRounding    = 0.0f;
    style.GrabRounding      = kFrameRounding;
    style.PopupRounding     = kFrameRounding;
    style.ScrollbarSize     = 12.0f;
    style.ScrollbarRounding = 4.0f;

    auto* colors                       = style.Colors;
    colors[ImGuiCol_WindowBg]          = ImColor(static_cast<int>(kBgPanel));
    colors[ImGuiCol_MenuBarBg]         = ImColor(static_cast<int>(kBgHeader));
    colors[ImGuiCol_Border]            = ImColor(static_cast<int>(kBorder));
    colors[ImGuiCol_Text]              = ImColor(static_cast<int>(kText));
    colors[ImGuiCol_TextDisabled]      = ImColor(static_cast<int>(kTextDim));
    colors[ImGuiCol_Button]            = ImColor(static_cast<int>(kBgPanel));
    colors[ImGuiCol_ButtonHovered]     = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_ButtonActive]      = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_Header]            = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_HeaderHovered]     = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_HeaderActive]      = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_FrameBg]           = ImColor(static_cast<int>(kBgPanel));
    colors[ImGuiCol_FrameBgHovered]    = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_FrameBgActive]     = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_TitleBg]           = ImColor(static_cast<int>(kBgHeader));
    colors[ImGuiCol_TitleBgActive]     = ImColor(static_cast<int>(kBgHeader));
    colors[ImGuiCol_TitleBgCollapsed]  = ImColor(static_cast<int>(kBgPanel));
    colors[ImGuiCol_CheckMark]         = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_SliderGrab]        = ImColor(static_cast<int>(kAccent));
    colors[ImGuiCol_SliderGrabActive]  = ImColor(static_cast<int>(kSelected));
    colors[ImGuiCol_Separator]         = ImColor(static_cast<int>(kBorder));
    colors[ImGuiCol_ResizeGrip]        = ImColor(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered] = ImColor(static_cast<int>(kHover));
    colors[ImGuiCol_ResizeGripActive]  = ImColor(static_cast<int>(kAccent));
}

} // namespace playback::editor::ui::theme
