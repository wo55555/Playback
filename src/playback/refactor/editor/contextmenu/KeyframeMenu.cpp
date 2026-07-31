#include "KeyframeMenu.h"

#include "imgui.h"

namespace playback::refactor::editor {

void KeyframeMenu::draw() {
    if (ImGui::BeginPopupContextItem("KeyframeContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Insert Before")) {}
        if (ImGui::MenuItem("Insert After")) {}

        ImGui::Separator();

        if (ImGui::MenuItem("Reset to Default")) {}
        if (ImGui::MenuItem("Copy Value")) {}
        if (ImGui::MenuItem("Paste Value")) {}

        ImGui::Separator();

        if (ImGui::MenuItem("Delete", "Del")) {}

        ImGui::EndPopup();
    }
}

} // namespace playback::refactor::editor