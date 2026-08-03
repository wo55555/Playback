#include "HintBar.h"

#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <array>
#include <string>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void HintBar::draw() {
    if (!mVisible) return;

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0x90, 0x90, 0x90, 0xff));

    // 6 most used shortcuts: Play / Add Marker / Split / Undo / Export / Help
    std::array<std::string, 6> const hints{
        std::string(ICON_PLAY) + "=" + "playback.refactorEditor.hints.play"_tr(),
        std::string(ICON_MARKER) + "=" + "playback.refactorEditor.hints.marker"_tr(),
        std::string(ICON_SPLIT) + "=" + "playback.refactorEditor.hints.split"_tr(),
        std::string(ICON_UNDO) + "=" + "playback.refactorEditor.hints.undo"_tr(),
        std::string(ICON_EXPORT) + "=" + "playback.refactorEditor.hints.export"_tr(),
        std::string(ICON_HELP) + "=" + "playback.refactorEditor.hints.help"_tr()
    };

    for (std::size_t i = 0; i < hints.size(); ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::TextUnformatted("   ");
        ImGui::SameLine();
        ImGui::TextUnformatted(hints[i].c_str());
    }

    ImGui::PopStyleColor();
}

void HintBar::toggle() { mVisible = !mVisible; }
void HintBar::setVisible(bool v) { mVisible = v; }

} // namespace playback::editor::ui
