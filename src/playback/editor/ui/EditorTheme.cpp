#include "EditorTheme.h"

#include "imgui.h"

namespace playback::editor::ui {

void EditorTheme::apply() const {
    auto& style = ImGui::GetStyle();

    style.WindowPadding     = {panelPadding, panelPadding};
    style.FramePadding      = {itemSpacing, itemSpacing};
    style.ItemSpacing       = {itemSpacing, itemSpacing};
    style.FrameRounding     = frameRounding;
    style.WindowRounding    = 0.0f;
    style.GrabRounding      = frameRounding;
    style.PopupRounding     = frameRounding;
    style.ScrollbarSize     = 12.0f;
    style.ScrollbarRounding = 4.0f;

    auto* colors                       = style.Colors;
    colors[ImGuiCol_WindowBg]          = ImColor(static_cast<int>(bgPanel));
    colors[ImGuiCol_MenuBarBg]         = ImColor(static_cast<int>(bgHeader));
    colors[ImGuiCol_Border]            = ImColor(static_cast<int>(border));
    colors[ImGuiCol_Text]              = ImColor(static_cast<int>(text));
    colors[ImGuiCol_TextDisabled]      = ImColor(static_cast<int>(textDim));
    colors[ImGuiCol_Button]            = ImColor(static_cast<int>(bgPanel));
    colors[ImGuiCol_ButtonHovered]     = ImColor(static_cast<int>(hover));
    colors[ImGuiCol_ButtonActive]      = ImColor(static_cast<int>(accent));
    colors[ImGuiCol_Header]            = ImColor(static_cast<int>(hover));
    colors[ImGuiCol_HeaderHovered]     = ImColor(static_cast<int>(hover));
    colors[ImGuiCol_HeaderActive]      = ImColor(static_cast<int>(accent));
    colors[ImGuiCol_FrameBg]           = ImColor(static_cast<int>(bgPanel));
    colors[ImGuiCol_FrameBgHovered]    = ImColor(static_cast<int>(hover));
    colors[ImGuiCol_FrameBgActive]     = ImColor(static_cast<int>(accent));
    colors[ImGuiCol_TitleBg]           = ImColor(static_cast<int>(bgHeader));
    colors[ImGuiCol_TitleBgActive]     = ImColor(static_cast<int>(bgHeader));
    colors[ImGuiCol_TitleBgCollapsed]  = ImColor(static_cast<int>(bgPanel));
    colors[ImGuiCol_CheckMark]         = ImColor(static_cast<int>(accent));
    colors[ImGuiCol_SliderGrab]        = ImColor(static_cast<int>(accent));
    colors[ImGuiCol_SliderGrabActive]  = ImColor(static_cast<int>(selected));
    colors[ImGuiCol_Separator]         = ImColor(static_cast<int>(border));
    colors[ImGuiCol_ResizeGrip]        = ImColor(0, 0, 0, 0);
    colors[ImGuiCol_ResizeGripHovered] = ImColor(static_cast<int>(hover));
    colors[ImGuiCol_ResizeGripActive]  = ImColor(static_cast<int>(accent));
}

} // namespace playback::editor::ui
