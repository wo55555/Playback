#include "ViewportMenu.h"

#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void ViewportMenu::draw(bool cameraEditingAvailable) {
    if (ImGui::BeginPopupContextItem("##viewport-context-menu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem(
            "playback.refactorEditor.context.viewport.addKeyframe"_tr().c_str(),
            "K",
            false,
            cameraEditingAvailable
        );
        ImGui::MenuItem("playback.refactorEditor.context.viewport.addMarker"_tr().c_str(), "M", false, false);
        ImGui::MenuItem("playback.refactorEditor.context.viewport.setPlayStart"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.viewport.setPlayEnd"_tr().c_str(), nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem(
            "playback.refactorEditor.menu.cameraPreset"_tr().c_str(),
            nullptr,
            false,
            cameraEditingAvailable
        );

        ImGui::Separator();

        ImGui::MenuItem(
            "playback.refactorEditor.context.viewport.copyCamera"_tr().c_str(),
            nullptr,
            false,
            cameraEditingAvailable
        );
        ImGui::MenuItem(
            "playback.refactorEditor.context.viewport.pasteCamera"_tr().c_str(),
            nullptr,
            false,
            cameraEditingAvailable
        );

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
