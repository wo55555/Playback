#include "StatusPanel.h"

#include "imgui.h"
#include "playback/editor/ui/ReplayEditor.h"

namespace playback::editor::ui {

void StatusPanel::draw() {
    const auto& state = ReplayEditor::getInstance().state();
    ImGui::TextUnformatted("[Replay]");
    ImGui::SameLine();
    ImGui::Text("Tick %d / %d", state.currentTick, state.totalTicks);
    ImGui::SameLine();
    ImGui::Text("%.2fx", state.playbackSpeed);
    ImGui::SameLine();
    ImGui::TextDisabled("Editing backends unavailable");
}

} // namespace playback::editor::ui
