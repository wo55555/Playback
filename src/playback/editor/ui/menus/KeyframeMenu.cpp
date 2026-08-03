#include "KeyframeMenu.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void KeyframeMenu::draw() {
    if (ImGui::BeginPopupContextItem("##keyframe-context-menu", ImGuiPopupFlags_MouseButtonRight)) {
        ImGui::MenuItem("playback.refactorEditor.context.keyframe.insertBefore"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.keyframe.insertAfter"_tr().c_str(), nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("playback.refactorEditor.context.keyframe.reset"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.keyframe.copyValue"_tr().c_str(), nullptr, false, false);
        ImGui::MenuItem("playback.refactorEditor.context.keyframe.pasteValue"_tr().c_str(), nullptr, false, false);

        ImGui::Separator();

        ImGui::MenuItem("playback.refactorEditor.menu.delete"_tr().c_str(), "Del", false, false);

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
