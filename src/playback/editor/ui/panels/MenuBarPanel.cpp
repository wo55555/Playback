#include "MenuBarPanel.h"

#include "playback/editor/ui/FormatUtils.h"
#include "playback/editor/ui/ReplayUILayout.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <string>

namespace playback::editor::ui {

namespace {

constexpr auto MenuBgColor       = IM_COL32(28, 28, 32, 255);
constexpr auto MenuTextColor     = IM_COL32(236, 236, 240, 255);
constexpr auto SeparatorColor    = IM_COL32(92, 92, 104, 255);
constexpr auto MenuHoveredColor  = IM_COL32(68, 68, 78, 255);
constexpr auto MenuSelectedColor = IM_COL32(82, 82, 94, 255);

} // namespace

void drawMenuBarPanel(EditorState const& state, ReplayUILayout const& layout, std::vector<EditorAction>& actions) {
    using ll::i18n_literals::operator""_tr;

    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x < 320.0f || io.DisplaySize.y < 200.0f) return;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, layout.menuBarHeight), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * layout.scale, 0.0f));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(7.0f * layout.scale, std::max(0.0f, (layout.menuBarHeight - ImGui::GetFontSize()) * 0.5f))
    );
    ImGui::PushStyleColor(ImGuiCol_WindowBg, MenuBgColor);
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, MenuBgColor);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, MenuBgColor);
    ImGui::PushStyleColor(ImGuiCol_Text, MenuTextColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MenuHoveredColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, MenuSelectedColor);
    ImGui::PushStyleColor(ImGuiCol_Separator, SeparatorColor);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                     | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;

    if (ImGui::Begin("##PlaybackReplayMenuBar", nullptr, flags)) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("playback.editor.menu.file"_tr().c_str())) {
                ImGui::MenuItem("playback.editor.menu.exportVideo"_tr().c_str(), nullptr, false, false);
                ImGui::MenuItem("playback.editor.menu.exportScreenshot"_tr().c_str(), nullptr, false, false);
                ImGui::Separator();
                ImGui::MenuItem("playback.editor.menu.openReplay"_tr().c_str(), nullptr, false, false);
                ImGui::MenuItem("playback.editor.menu.openRecent"_tr().c_str(), nullptr, false, false);
                ImGui::Separator();
                if (ImGui::MenuItem("playback.editor.menu.exitReplay"_tr().c_str()))
                    actions.push_back({EditorActionType::StopReplay});
                ImGui::EndMenu();
            }

            ImGui::MenuItem("playback.editor.menu.preferences"_tr().c_str(), nullptr, false, false);
            ImGui::Separator();

            ImGui::MenuItem("playback.editor.menu.playerList"_tr().c_str(), nullptr, false, false);
            ImGui::MenuItem("playback.editor.menu.movement"_tr().c_str(), nullptr, false, false);
            ImGui::MenuItem("playback.editor.menu.renderFilter"_tr().c_str(), nullptr, false, false);
            ImGui::MenuItem("playback.editor.menu.keybinds"_tr().c_str(), nullptr, false, false);
            ImGui::Separator();
            ImGui::MenuItem("playback.editor.menu.hideReplayUi"_tr().c_str(), nullptr, false, false);

            std::string const status =
                std::string("Playback  |  ")
                + (state.paused ? "playback.editor.status.paused"_tr() : "playback.editor.status.playing"_tr())
                + "  |  " + utils::formatTimestamp(state.currentTick) + " / "
                + utils::formatTimestamp(state.totalTicks);
            float const statusWidth = ImGui::CalcTextSize(status.c_str()).x;
            float const statusX     = ImGui::GetWindowWidth() - statusWidth - 10.0f * layout.scale;
            if (statusX > ImGui::GetCursorPosX() + 12.0f * layout.scale) {
                ImGui::SetCursorPosX(statusX);
                ImGui::TextDisabled("%s", status.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(5);
}

} // namespace playback::editor::ui
