#include "ClipMenu.h"

#include "imgui.h"

namespace playback::refactor::editor {

void ClipMenu::draw() {
    if (ImGui::BeginPopupContextItem("ClipContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Cut", "Ctrl+X")) {}
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {}
        if (ImGui::MenuItem("Paste", "Ctrl+V")) {}

        ImGui::Separator();

        if (ImGui::MenuItem("Split at Playhead", "Ctrl+K")) {}
        if (ImGui::MenuItem("Trim Left to Playhead")) {}
        if (ImGui::MenuItem("Trim Right to Playhead")) {}

        ImGui::Separator();

        if (ImGui::MenuItem("Properties")) {}
        if (ImGui::MenuItem("Delete", "Del")) {}

        ImGui::EndPopup();
    }
}

} // namespace playback::refactor::editor