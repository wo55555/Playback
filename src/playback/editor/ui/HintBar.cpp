#include "HintBar.h"

#include "playback/editor/ui/iconfont.h"

#include "imgui.h"

namespace playback::editor::ui {

void HintBar::draw() {
    if (!mVisible) return;

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0x90, 0x90, 0x90, 0xff));

    // 6 most used shortcuts: Play / Add Marker / Split / Undo / Export / Help
    const char* hints[] = {
        ICON_PLAY "=play",
        ICON_MARKER "=marker",
        ICON_SPLIT "=split",
        ICON_UNDO "=undo",
        ICON_EXPORT "=export",
        ICON_HELP "=help"
    };

    for (int i = 0; i < 6; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::TextUnformatted("   ");
        ImGui::SameLine();
        ImGui::TextUnformatted(hints[i]);
    }

    ImGui::PopStyleColor();
}

void HintBar::toggle() { mVisible = !mVisible; }
void HintBar::setVisible(bool v) { mVisible = v; }

} // namespace playback::editor::ui