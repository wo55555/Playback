#include "TrackHeaderMenu.h"

#include "imgui.h"

namespace playback::editor::ui {

void TrackHeaderMenu::draw() {
    if (ImGui::BeginPopupContextItem("TrackHeaderContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem("Rename", nullptr, false, false);
        ImGui::MenuItem("Delete Track", nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("Lock / Unlock", nullptr, false, false);
        ImGui::MenuItem("Mute / Unmute", nullptr, false, false);
        ImGui::MenuItem("Hide / Show", nullptr, false, false);

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
