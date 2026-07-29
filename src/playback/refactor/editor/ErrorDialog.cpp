#include "ErrorDialog.h"

#include "imgui.h"

namespace playback::refactor::editor {

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

    ImGui::OpenPopup("Export Failed");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 200));

    if (ImGui::BeginPopupModal("Export Failed", &mOpen, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s", mMsg.c_str());

        ImGui::Separator();
        ImGui::TextDisabled("(no log view; details in console)");

        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            mOpen = false;
        }

        ImGui::EndPopup();
    }
}

} // namespace playback::refactor::editor