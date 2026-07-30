#include "EditMode.h"

#include "playback/refactor/editor/Editor.h"

#include "imgui.h"

#include <algorithm>

namespace playback::refactor::editor {

void EditMode::draw() {
    auto& editor = Editor::getInstance();

    constexpr float kMenuHeight   = 24.0f;
    constexpr float kStatusHeight = 22.0f;
    constexpr float kCurveWidth   = 280.0f;
    constexpr float kSplitterThickness = 4.0f;
    constexpr float kDetailsMinWidth = 220.0f;
    constexpr float kViewportMinWidth = 320.0f;
    constexpr float kViewportMinHeight = 180.0f;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float contentHeight = std::max(1.0f, displaySize.y - kMenuHeight - kStatusHeight);
    float maxDetailsRatio = std::min(0.50f, 1.0f - kViewportMinWidth / std::max(1.0f, displaySize.x));
    float minDetailsRatio = std::min(kDetailsMinWidth / std::max(1.0f, displaySize.x), maxDetailsRatio);
    editor.mDetailsWidthRatio = std::clamp(editor.mDetailsWidthRatio, minDetailsRatio, maxDetailsRatio);
    float detailsWidth = displaySize.x * editor.mDetailsWidthRatio;
    float leftWidth = displaySize.x - detailsWidth;
    float maxTimelineRatio = std::min(0.65f, 1.0f - kViewportMinHeight / contentHeight);
    float minTimelineRatio = std::min(0.18f, maxTimelineRatio);
    editor.mTimelineHeightRatio = std::clamp(editor.mTimelineHeightRatio, minTimelineRatio, maxTimelineRatio);
    float timelineHeight = contentHeight * editor.mTimelineHeightRatio;
    float viewportHeight = contentHeight - timelineHeight - kSplitterThickness;

    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::Begin("##MenuBar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar);
        editor.mMenuBar.draw();
        ImGui::End();
    }

    float curveWidth = 0.0f;
    if (editor.mCurveEditorPanel.isOpen()) {
        curveWidth = kCurveWidth + kSplitterThickness;
    }

    {
        float detailsX = displaySize.x - detailsWidth;
        float detailsY = kMenuHeight;
        float detailsH = contentHeight;

        ImGui::SetNextWindowPos(ImVec2(detailsX, detailsY));
        ImGui::SetNextWindowSize(ImVec2(detailsWidth, detailsH));
        ImGui::Begin("##DetailsPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mDetailsPanel.draw();
        ImGui::End();
    }

    if (editor.mCurveEditorPanel.isOpen()) {
        float curveX = leftWidth - curveWidth;
        float curveY = kMenuHeight;
        float curveH = contentHeight;
        ImGui::SetNextWindowPos(ImVec2(curveX, curveY));
        ImGui::SetNextWindowSize(ImVec2(kCurveWidth, curveH));
        ImGui::GetForegroundDrawList()->AddLine(
            ImVec2(curveX - 1, curveY), ImVec2(curveX - 1, curveY + curveH),
            IM_COL32(0x5a, 0x5a, 0x5a, 0xff));
        ImGui::Begin("##CurveEditorPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mCurveEditorPanel.draw();
        ImGui::End();
    }

    float workspaceWidth = leftWidth - curveWidth;

    {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(workspaceWidth, viewportHeight));
        ImGui::Begin("##ViewportPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mViewportPanel.draw();
        ImGui::End();
    }

    {
        float timelineY = kMenuHeight + viewportHeight + kSplitterThickness;
        ImGui::SetNextWindowPos(ImVec2(0, timelineY));
        ImGui::SetNextWindowSize(ImVec2(workspaceWidth, timelineHeight));
        ImGui::Begin("##TimelinePanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mTimelinePanel.draw();
        ImGui::End();
    }

    {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, contentHeight));
        ImGui::Begin("##LayoutSplitters", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        Rect fullArea{{0.0f, kMenuHeight}, {displaySize.x, displaySize.y - kStatusHeight}};
        Rect leftArea{{0.0f, kMenuHeight}, {leftWidth, displaySize.y - kStatusHeight}};
        editor.mDetailsWidthRatio = editor.mSplitter.drawVerticalSplit(
            editor.mDetailsWidthRatio, fullArea, minDetailsRatio, maxDetailsRatio);
        editor.mTimelineHeightRatio = editor.mSplitter.drawHorizontalSplit(
            1.0f - editor.mTimelineHeightRatio, leftArea, 1.0f - maxTimelineRatio, 1.0f - minTimelineRatio);
        editor.mTimelineHeightRatio = 1.0f - editor.mTimelineHeightRatio;
        ImGui::End();
    }

    {
        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin("##StatusPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mStatusPanel.draw();
        ImGui::End();
    }
}

} // namespace playback::refactor::editor
