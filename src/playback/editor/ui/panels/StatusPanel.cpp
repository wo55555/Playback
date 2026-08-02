#include "StatusPanel.h"

#include "playback/editor/ui/ReplayEditor.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void StatusPanel::draw() {
    const auto& state = ReplayEditor::getInstance().state();
    ImGui::TextUnformatted("playback.refactorEditor.status.replay"_tr().c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted("playback.refactorEditor.status.tick"_tr(state.currentTick, state.totalTicks).c_str());
    ImGui::SameLine();
    ImGui::Text("%.2fx", state.playbackSpeed);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", "playback.refactorEditor.status.backendsUnavailable"_tr().c_str());
}

} // namespace playback::editor::ui
