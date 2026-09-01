#include "playback/editor/ui/components/Widgets.h"

#include "playback/editor/ui/EditorTheme.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace playback::editor::ui::widgets {

using namespace ll::i18n_literals;

std::string formatTick(int tick) {
    char value[32]{};
    tick                   = std::max(0, tick);
    int const totalSeconds = tick / kTicksPerSecond;
    int const centiseconds = tick % kTicksPerSecond * (100 / kTicksPerSecond);
    std::snprintf(value, sizeof(value), "%02d:%02d.%02d", totalSeconds / 60, totalSeconds % 60, centiseconds);
    return value;
}

float iconButtonSize() { return std::max(25.0f, ImGui::GetFontSize() + 12.0f); }

bool iconButton(char const* id, char const* icon, char const* tooltip, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    float const  buttonSize = iconButtonSize();
    ImVec2 const cursor     = ImGui::GetCursorScreenPos();
    ImVec2 const mouse      = ImGui::GetMousePos();
    bool const   hovered    = enabled && mouse.x >= cursor.x && mouse.x <= cursor.x + buttonSize && mouse.y >= cursor.y
                      && mouse.y <= cursor.y + buttonSize;
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? theme::kIconHighlight : theme::kIconInactive);
    bool const clicked = ImGui::Button((std::string(icon) + "##" + id).c_str(), {buttonSize, buttonSize});
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::EndDisabled();
    return clicked;
}

void noActiveProjectPlaceholder() {
    ImGui::TextDisabled("%s", "playback.refactorEditor.common.noActiveProject"_tr().c_str());
}

} // namespace playback::editor::ui::widgets
