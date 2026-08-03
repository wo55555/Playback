#include "ClipMenu.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void ClipMenu::draw() {
    if (ImGui::BeginPopupContextItem("##clip-context-menu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem("playback.refactorEditor.context.clip.cut"_tr().c_str(), "Ctrl+X", false, false);
        ImGui::MenuItem("playback.refactorEditor.context.clip.copy"_tr().c_str(), "Ctrl+C", false, false);
        ImGui::MenuItem("playback.refactorEditor.context.clip.paste"_tr().c_str(), "Ctrl+V", false, false);

        ImGui::Separator();

        ImGui::MenuItem("playback.refactorEditor.context.clip.split"_tr().c_str(), "Ctrl+K", false, false);
        ImGui::MenuItem("playback.refactorEditor.context.clip.trimLeft"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.clip.trimRight"_tr().c_str(), nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("playback.refactorEditor.context.clip.properties"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.menu.delete"_tr().c_str(), "Del", false, false);

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
