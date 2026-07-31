#include "StatusPanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/ModeManager.h"

#include "imgui.h"

namespace playback::refactor::editor {

void StatusPanel::draw() {
    const auto& state = Editor::getInstance().state();
    const auto& mode  = ModeManager::getInstance();

    // Mode text
    if (mode.current() == EditorMode::Edit) {
        ImGui::Text("[Edit]");
    } else {
        ImGui::TextColored(ImVec4(0.23f, 0.55f, 0.94f, 1.0f), "[Render]");
    }

    ImGui::SameLine();

    // Project name
    ImGui::Text("%s", state.projectName.empty() ? "(untitled)" : state.projectName.c_str());

    ImGui::SameLine();

    // FPS
    char fpsStr[32];
    std::snprintf(fpsStr, sizeof(fpsStr), "%.0f FPS", state.fps);
    ImGui::Text("%s", fpsStr);

    ImGui::SameLine();

    // Memory
    char memStr[32];
    std::snprintf(memStr, sizeof(memStr), "%.0f MB", static_cast<float>(state.memoryUsageBytes) / (1024.0f * 1024.0f));
    ImGui::Text("%s", memStr);
}

} // namespace playback::refactor::editor