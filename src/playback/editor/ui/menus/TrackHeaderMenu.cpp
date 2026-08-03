#include "TrackHeaderMenu.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void TrackHeaderMenu::draw() {
    if (ImGui::BeginPopupContextItem("##track-header-context-menu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem("playback.refactorEditor.context.track.rename"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.track.delete"_tr().c_str(), nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("playback.refactorEditor.context.track.toggleLock"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.track.toggleMute"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.track.toggleVisibility"_tr().c_str(), nullptr, false, false);

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
