#include "TrackHeaderMenu.h"

#include "imgui.h"

namespace playback::refactor::editor {

void TrackHeaderMenu::draw() {
    if (ImGui::BeginPopupContextItem("TrackHeaderContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Rename")) {}
        if (ImGui::MenuItem("Delete Track")) {}

        ImGui::Separator();

        if (ImGui::MenuItem("Lock / Unlock")) {}
        if (ImGui::MenuItem("Mute / Unmute")) {}
        if (ImGui::MenuItem("Hide / Show")) {}

        ImGui::EndPopup();
    }
}

} // namespace playback::refactor::editor