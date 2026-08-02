#include "ClipMenu.h"

#include "imgui.h"

namespace playback::editor::ui {

void ClipMenu::draw() {
    if (ImGui::BeginPopupContextItem("ClipContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem("Cut", "Ctrl+X", false, false);
        ImGui::MenuItem("Copy", "Ctrl+C", false, false);
        ImGui::MenuItem("Paste", "Ctrl+V", false, false);

        ImGui::Separator();

        ImGui::MenuItem("Split at Playhead", "Ctrl+K", false, false);
        ImGui::MenuItem("Trim Left to Playhead", nullptr, false, false);
        ImGui::MenuItem("Trim Right to Playhead", nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("Properties", nullptr, false, false);
        ImGui::MenuItem("Delete", "Del", false, false);

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
