#include "EditMode.h"

#include "playback/refactor/editor/Editor.h"

#include "imgui.h"

namespace playback::refactor::editor {

void EditMode::draw() {
    auto& editor = Editor::getInstance();

    // Layout constants
    constexpr float kMenuHeight   = 24.0f;
    constexpr float kStatusHeight = 22.0f;
    constexpr float kDetailsWidth = 320.0f;
    constexpr float kCurveWidth   = 280.0f;
    constexpr float kSplitterH    = 4.0f;

    // Get display size
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // === Menu ===
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::Begin("##MenuBar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar);
        editor.mMenuBar.draw();
        ImGui::End();
    }

    // Right side width: details panel + optional curve editor
    float rightW = kDetailsWidth;
    if (editor.mCurveEditorPanel.isOpen()) {
        rightW += kCurveWidth + kSplitterH;
    }

    // === Details (rightmost) ===
    {
        float detailsX = displaySize.x - kDetailsWidth;
        float detailsY = kMenuHeight;
        float detailsH = displaySize.y - kMenuHeight - kStatusHeight;

        // Shift details left if curve editor is open
        if (editor.mCurveEditorPanel.isOpen()) {
            detailsX -= kCurveWidth + kSplitterH;
        }

        ImGui::SetNextWindowPos(ImVec2(detailsX, detailsY));
        ImGui::SetNextWindowSize(ImVec2(kDetailsWidth, detailsH));
        ImGui::Begin("##DetailsPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mDetailsPanel.draw();
        ImGui::End();
    }

    // === Curve Editor (between viewport and details) ===
    if (editor.mCurveEditorPanel.isOpen()) {
        float curveX = displaySize.x - kDetailsWidth - kSplitterH - kCurveWidth;
        float curveY = kMenuHeight;
        float curveH = displaySize.y - kMenuHeight - kStatusHeight;
        ImGui::SetNextWindowPos(ImVec2(curveX, curveY));
        ImGui::SetNextWindowSize(ImVec2(kCurveWidth, curveH));
        // Use a separator border
        ImGui::GetForegroundDrawList()->AddLine(
            ImVec2(curveX - 1, curveY), ImVec2(curveX - 1, curveY + curveH),
            IM_COL32(0x5a, 0x5a, 0x5a, 0xff));
        ImGui::Begin("##CurveEditorPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mCurveEditorPanel.draw();
        ImGui::End();
    }

    // === Left area (Viewport + Timeline) ===
    float leftW = displaySize.x - rightW;

    // Viewport (top portion of left area)
    {
        float viewportH = (displaySize.y - kMenuHeight - kStatusHeight) * (1.0f - editor.mTimelineRatio) - kSplitterH;
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(leftW, viewportH));
        ImGui::Begin("##ViewportPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mViewportPanel.draw();
        ImGui::End();
    }

    // Splitter
    {
        float splitterY = kMenuHeight + (displaySize.y - kMenuHeight - kStatusHeight) * (1.0f - editor.mTimelineRatio);
        ImGui::GetForegroundDrawList()->AddLine(
            ImVec2(0, splitterY), ImVec2(leftW, splitterY),
            IM_COL32(0x5a, 0x5a, 0x5a, 0xff));
    }

    // Timeline (bottom portion of left area)
    {
        float timelineY = kMenuHeight + (displaySize.y - kMenuHeight - kStatusHeight) * (1.0f - editor.mTimelineRatio);
        float timelineH = (displaySize.y - kMenuHeight - kStatusHeight) * editor.mTimelineRatio;
        ImGui::SetNextWindowPos(ImVec2(0, timelineY));
        ImGui::SetNextWindowSize(ImVec2(leftW, timelineH));
        ImGui::Begin("##TimelinePanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
        editor.mTimelinePanel.draw();
        ImGui::End();
    }

    // === Status ===
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