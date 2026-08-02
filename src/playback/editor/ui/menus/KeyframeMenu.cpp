#include "KeyframeMenu.h"

#include "imgui.h"

namespace playback::editor::ui {

void KeyframeMenu::draw() {
    if (ImGui::BeginPopupContextItem("KeyframeContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem("Insert Before", nullptr, false, false);
        ImGui::MenuItem("Insert After", nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("Reset to Default", nullptr, false, false);
        ImGui::MenuItem("Copy Value", nullptr, false, false);
        ImGui::MenuItem("Paste Value", nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("Delete", "Del", false, false);

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
