#include "ViewportMenu.h"

#include "playback/editor/ui/iconfont.h"

#include "imgui.h"

namespace playback::editor::ui {

void ViewportMenu::draw(bool cameraEditingAvailable) {
    if (ImGui::BeginPopupContextItem("ViewportContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem("Add Keyframe Here", "K", false, cameraEditingAvailable);
        ImGui::MenuItem("Add Marker Here", "M", false, false);
        ImGui::MenuItem("Set as Play Start", nullptr, false, false);
        ImGui::MenuItem("Set as Play End", nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("Camera Preset", nullptr, false, cameraEditingAvailable);

        ImGui::Separator();

        ImGui::MenuItem("Copy Camera State", nullptr, false, cameraEditingAvailable);
        ImGui::MenuItem("Paste Camera State", nullptr, false, cameraEditingAvailable);

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
