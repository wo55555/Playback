#include "DetailsPanel.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

void drawUnavailableSection(char const* icon, char const* title, bool available) {
    ImGui::Text("%s  %s", icon, title);
    ImGui::Separator();
    ImGui::BeginDisabled(!available);
    ImGui::Button(ICON_ADD, {28.0f, 28.0f});
    ImGui::SameLine();
    ImGui::TextUnformatted(
        (available ? "playback.refactorEditor.details.ready"_tr()
                   : "playback.refactorEditor.details.backendUnavailable"_tr())
            .c_str()
    );
    ImGui::EndDisabled();
}

} // namespace

void DetailsPanel::draw() {
    auto const& capabilities = ReplayEditor::getInstance().state().capabilities;

    ImGui::TextUnformatted("playback.refactorEditor.details.inspector"_tr().c_str());
    ImGui::Spacing();
    drawUnavailableSection(
        ICON_CAMERA,
        "playback.refactorEditor.details.cameraEditing"_tr().c_str(),
        capabilities.cameraEditing
    );
    ImGui::Spacing();
    drawUnavailableSection(
        ICON_VIDEO,
        "playback.refactorEditor.details.videoEditing"_tr().c_str(),
        capabilities.videoEditing
    );
    ImGui::Spacing();
    drawUnavailableSection(
        ICON_EXPORT,
        "playback.refactorEditor.details.videoExport"_tr().c_str(),
        capabilities.videoExport
    );
}

} // namespace playback::editor::ui
