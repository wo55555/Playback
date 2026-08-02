#include "DetailsPanel.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "imgui.h"

namespace playback::editor::ui {

namespace {

void drawUnavailableSection(char const* icon, char const* title, bool available) {
    ImGui::Text("%s  %s", icon, title);
    ImGui::Separator();
    ImGui::BeginDisabled(!available);
    ImGui::Button(ICON_ADD, {28.0f, 28.0f});
    ImGui::SameLine();
    ImGui::TextUnformatted(available ? "Ready" : "Backend unavailable");
    ImGui::EndDisabled();
}

} // namespace

void DetailsPanel::draw() {
    auto const& capabilities = ReplayEditor::getInstance().state().capabilities;

    ImGui::TextUnformatted("Inspector");
    ImGui::Spacing();
    drawUnavailableSection(ICON_CAMERA, "Camera editing", capabilities.cameraEditing);
    ImGui::Spacing();
    drawUnavailableSection(ICON_VIDEO, "Video editing", capabilities.videoEditing);
    ImGui::Spacing();
    drawUnavailableSection(ICON_EXPORT, "Video export", capabilities.videoExport);
}

} // namespace playback::editor::ui
