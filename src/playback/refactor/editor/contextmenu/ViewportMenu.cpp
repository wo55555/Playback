#include "ViewportMenu.h"

#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"

namespace playback::refactor::editor {

void ViewportMenu::draw() {
    if (ImGui::BeginPopupContextWindow("ViewportContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Add Keyframe Here", "K")) {}
        if (ImGui::MenuItem("Add Marker Here", "M")) {}
        if (ImGui::MenuItem("Set as Play Start")) {}
        if (ImGui::MenuItem("Set as Play End")) {}

        ImGui::Separator();

        if (ImGui::BeginMenu("Camera Preset")) {
            ImGui::MenuItem("First Person");
            ImGui::MenuItem("Third Person");
            ImGui::MenuItem("Free");
            ImGui::MenuItem("Follow Entity");
            ImGui::MenuItem("Orbit");
            ImGui::MenuItem("Telephoto");
            ImGui::MenuItem("Drone");
            ImGui::EndMenu();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Copy Camera State")) {}
        if (ImGui::MenuItem("Paste Camera State")) {}

        ImGui::EndPopup();
    }
}

} // namespace playback::refactor::editor