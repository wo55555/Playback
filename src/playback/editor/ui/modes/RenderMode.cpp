#include "RenderMode.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/editor/ui/modes/ModeManager.h"

#include "imgui.h"

namespace playback::editor::ui {

void RenderMode::draw() {
    auto& editor = ReplayEditor::getInstance();
    constexpr float kMenuHeight   = 24.0f;
    constexpr float kStatusHeight = 22.0f;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // === Menu ===
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::Begin("##RenderMenuBar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar);
        editor.mMenuBar.draw();
        ImGui::End();
    }

    // === Render card (centered) ===
    {
        float cardWidth  = 480.0f;
        float cardHeight = 360.0f;
        float cardX      = (displaySize.x - cardWidth) * 0.5f;
        float cardY      = (displaySize.y - cardHeight) * 0.5f;

        ImGui::SetNextWindowPos(ImVec2(cardX, cardY));
        ImGui::SetNextWindowSize(ImVec2(cardWidth, cardHeight));
        ImGui::Begin("##RenderCard", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);

        // Centered render icon
        float iconSize = 48.0f;
        ImGui::SetCursorPosX((cardWidth - iconSize) * 0.5f);
        ImGui::Text("%s", ICON_RENDER);

        // Title
        ImGui::SetCursorPosX((cardWidth - 120.0f) * 0.5f);
        ImGui::Text("Rendering...");

        ImGui::Spacing();
        ImGui::Spacing();

        // Progress bar
        float progress = (mTotalFrames > 0) ? static_cast<float>(mCurrentFrame) / static_cast<float>(mTotalFrames) : 0.0f;
        char progressLabel[32];
        std::snprintf(progressLabel, sizeof(progressLabel), "%d%%", mProgressPercent);

        ImGui::ProgressBar(progress, ImVec2(cardWidth - 40, 24.0f), progressLabel);
        ImGui::Spacing();

        // Frame info
        char frameInfo[64];
        std::snprintf(frameInfo, sizeof(frameInfo), "Frame %d / %d", mCurrentFrame, mTotalFrames);
        ImGui::SetCursorPosX((cardWidth - 160.0f) * 0.5f);
        ImGui::Text("%s", frameInfo);

        // Format info
        ImGui::SetCursorPosX((cardWidth - 200.0f) * 0.5f);
        ImGui::Text("1920x1080 60fps H.264");

        // ETA
        ImGui::SetCursorPosX((cardWidth - 100.0f) * 0.5f);
        ImGui::Text("ETA %s", mEta.empty() ? "0:00:00" : mEta.c_str());

        // Output path
        ImGui::Spacing();
        ImGui::SetCursorPosX((cardWidth - 300.0f) * 0.5f);
        ImGui::Text("Output: %s", mOutputPath.empty() ? "D:\\exports\\replay-001.mp4" : mOutputPath.c_str());

        ImGui::Spacing();
        ImGui::Spacing();

        // Cancel button
        ImGui::SetCursorPosX((cardWidth - 160.0f) * 0.5f);
        if (ImGui::Button("Cancel Render", ImVec2(160.0f, 32.0f))) {
            ModeManager::getInstance().switchTo(EditorMode::Edit);
        }

        ImGui::End();
    }

    // === Status ===
    {
        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin("##RenderStatus", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mStatusPanel.draw();
        ImGui::End();
    }
}

} // namespace playback::editor::ui