#include "RenderMode.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/editor/ui/modes/ModeManager.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void RenderMode::draw() {
    auto&       editor        = ReplayEditor::getInstance();
    float const uiScale       = std::max(1.0f, ImGui::GetIO().FontGlobalScale);
    float const kMenuHeight   = 30.0f * uiScale;
    float const kStatusHeight = 22.0f * uiScale;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::Begin(
            "##RenderMenuBar",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar
        );
        editor.mMenuBar.draw();
        ImGui::End();
    }

    {
        float cardWidth  = 480.0f;
        float cardHeight = 360.0f;
        float cardX      = (displaySize.x - cardWidth) * 0.5f;
        float cardY      = (displaySize.y - cardHeight) * 0.5f;

        ImGui::SetNextWindowPos(ImVec2(cardX, cardY));
        ImGui::SetNextWindowSize(ImVec2(cardWidth, cardHeight));
        ImGui::Begin(
            "##RenderCard",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );

        float iconSize = 48.0f;
        ImGui::SetCursorPosX((cardWidth - iconSize) * 0.5f);
        ImGui::Text("%s", ICON_RENDER);

        std::string const rendering = "playback.refactorEditor.render.rendering"_tr();
        ImGui::SetCursorPosX((cardWidth - ImGui::CalcTextSize(rendering.c_str()).x) * 0.5f);
        ImGui::TextUnformatted(rendering.c_str());

        ImGui::Spacing();
        ImGui::Spacing();

        float progress =
            (mTotalFrames > 0) ? static_cast<float>(mCurrentFrame) / static_cast<float>(mTotalFrames) : 0.0f;
        char progressLabel[32];
        std::snprintf(progressLabel, sizeof(progressLabel), "%d%%", mProgressPercent);

        ImGui::ProgressBar(progress, ImVec2(cardWidth - 40, 24.0f), progressLabel);
        ImGui::Spacing();

        std::string const frameInfo = "playback.refactorEditor.render.frame"_tr(mCurrentFrame, mTotalFrames);
        ImGui::SetCursorPosX((cardWidth - 160.0f) * 0.5f);
        ImGui::TextUnformatted(frameInfo.c_str());

        ImGui::SetCursorPosX((cardWidth - 200.0f) * 0.5f);
        ImGui::Text("1920x1080 60fps H.264");

        ImGui::SetCursorPosX((cardWidth - 100.0f) * 0.5f);
        ImGui::TextUnformatted("playback.refactorEditor.render.eta"_tr(mEta.empty() ? "0:00:00" : mEta).c_str());

        ImGui::Spacing();
        ImGui::SetCursorPosX((cardWidth - 300.0f) * 0.5f);
        std::string const outputPath = mOutputPath.empty() ? "D:\\exports\\replay-001.mp4" : mOutputPath;
        ImGui::TextUnformatted("playback.refactorEditor.render.output"_tr(outputPath).c_str());

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::SetCursorPosX((cardWidth - 160.0f) * 0.5f);
        if (ImGui::Button("playback.refactorEditor.render.cancel"_tr().c_str(), ImVec2(160.0f, 32.0f))) {
            ModeManager::getInstance().switchTo(EditorMode::Edit);
        }

        ImGui::End();
    }

    {
        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin(
            "##RenderStatus",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );
        editor.mStatusPanel.draw();
        ImGui::End();
    }
}

} // namespace playback::editor::ui
