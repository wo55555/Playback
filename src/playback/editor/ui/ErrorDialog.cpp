#include "ErrorDialog.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

ErrorDialog& ErrorDialog::getInstance() {
    static ErrorDialog instance;
    return instance;
}

void ErrorDialog::show(std::string_view title, std::string_view msg) {
    mTitle = title;
    mMsg   = msg;
    mOpen  = true;
}

void ErrorDialog::draw() {
    if (!mOpen) return;

    std::string const title =
        (mTitle.empty() ? "playback.refactorEditor.error.exportFailed"_tr() : mTitle) + "###editor-error";
    ImGui::OpenPopup(title.c_str());
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 200));

    if (ImGui::BeginPopupModal(title.c_str(), &mOpen, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", mMsg.c_str());

        ImGui::Separator();
        ImGui::TextDisabled("%s", "playback.refactorEditor.error.detailsInConsole"_tr().c_str());

        ImGui::Spacing();
        if (ImGui::Button("playback.refactorEditor.common.ok"_tr().c_str(), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            mOpen = false;
        }

        ImGui::EndPopup();
    }
}

} // namespace playback::editor::ui
