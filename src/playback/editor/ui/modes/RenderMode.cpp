#include "RenderMode.h"

#include "playback/editor/ui/ReplayEditor.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

void RenderMode::draw() {
    auto&       editor        = ReplayEditor::getInstance();
    auto const& status        = editor.state().exportStatus;
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
        float cardHeight = 390.0f;
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

        auto const  completedFrames = std::min(status.writtenFrames, status.totalFrames);
        float const progress        = status.totalFrames > 0
                                        ? static_cast<float>(completedFrames) / static_cast<float>(status.totalFrames)
                                        : 0.0f;
        char        progressLabel[32];
        std::snprintf(progressLabel, sizeof(progressLabel), "%d%%", static_cast<int>(progress * 100.0f));

        ImGui::ProgressBar(progress, ImVec2(cardWidth - 40, 24.0f), progressLabel);
        ImGui::Spacing();

        std::string const frameInfo = "playback.refactorEditor.render.frame"_tr(completedFrames, status.totalFrames);
        ImGui::SetCursorPosX((cardWidth - 160.0f) * 0.5f);
        ImGui::TextUnformatted(frameInfo.c_str());

        std::string const format = status.format == exporting::ExportFormat::Mp4Video
                                     ? "playback.refactorEditor.export.mp4"_tr()
                                     : "playback.refactorEditor.export.pngSequence"_tr();
        ImGui::SetCursorPosX((cardWidth - ImGui::CalcTextSize(format.c_str()).x) * 0.5f);
        ImGui::TextUnformatted(format.c_str());

        ImGui::Spacing();
        ImGui::SetCursorPosX((cardWidth - 300.0f) * 0.5f);
        auto const        outputUtf8 = status.outputPath.generic_u8string();
        std::string const outputPath{reinterpret_cast<char const*>(outputUtf8.data()), outputUtf8.size()};
        ImGui::PushTextWrapPos(cardWidth - 20.0f);
        ImGui::TextWrapped("%s", "playback.refactorEditor.render.output"_tr(outputPath).c_str());
        ImGui::PopTextWrapPos();

        if (!status.latestFramePath.empty()) {
            auto const        latestUtf8 = status.latestFramePath.filename().generic_u8string();
            std::string const latestFrame{reinterpret_cast<char const*>(latestUtf8.data()), latestUtf8.size()};
            ImGui::TextWrapped("%s", "playback.refactorEditor.render.latestFrame"_tr(latestFrame).c_str());
        }

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::SetCursorPosX((cardWidth - 160.0f) * 0.5f);
        bool const cancelling = status.state == exporting::ExportState::Cancelling;
        ImGui::BeginDisabled(cancelling);
        if (ImGui::Button(
                (cancelling ? "playback.refactorEditor.render.cancelling"_tr()
                            : "playback.refactorEditor.render.cancel"_tr())
                    .c_str(),
                ImVec2(160.0f, 32.0f)
            )) {
            editor.submitAction({EditorActionType::CancelExport});
        }
        ImGui::EndDisabled();

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
