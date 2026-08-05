#include "EditMode.h"

#include "playback/editor/ui/ReplayEditor.h"

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui {

void EditMode::draw() {
    auto& editor = ReplayEditor::getInstance();

    float const fontSize = ImGui::GetFontSize();
    float const kMenuHeight = fontSize + 14.0f;
    float const kStatusHeight = fontSize + 10.0f;
    float const kCurveWidth = std::max(280.0f, fontSize * 16.0f);
    float const kSplitterThickness = 4.0f;
    float const kDetailsMinWidth = std::max(260.0f, fontSize * 15.0f);
    float const kViewportMinWidth = std::max(320.0f, fontSize * 22.0f);
    float const kViewportMinHeight = std::max(180.0f, fontSize * 12.0f);
    float const kTimelineMinHeight = fontSize * 9.0f;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float contentHeight = std::max(1.0f, displaySize.y - kMenuHeight - kStatusHeight);
    float curveReservedWidth = editor.mCurveEditorPanel.isOpen() ? kCurveWidth + kSplitterThickness : 0.0f;
    float maxDetailsRatio = std::min(
        0.50f,
        1.0f - (kViewportMinWidth + curveReservedWidth) / std::max(1.0f, displaySize.x));
    float minDetailsRatio = std::min(kDetailsMinWidth / std::max(1.0f, displaySize.x), maxDetailsRatio);
    editor.mDetailsWidthRatio = std::clamp(editor.mDetailsWidthRatio, minDetailsRatio, maxDetailsRatio);
    float detailsWidth = displaySize.x * editor.mDetailsWidthRatio;
    float leftWidth = displaySize.x - detailsWidth;
    float maxTimelineRatio = std::min(0.70f, 1.0f - kViewportMinHeight / contentHeight);
    float minTimelineRatio = std::min(kTimelineMinHeight / contentHeight, maxTimelineRatio);
    editor.mTimelineHeightRatio = std::clamp(editor.mTimelineHeightRatio, minTimelineRatio, maxTimelineRatio);
    float timelineHeight        = contentHeight * editor.mTimelineHeightRatio;
    float viewportHeight        = contentHeight - timelineHeight - kSplitterThickness;

    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::Begin(
            "##EditorMenuBar",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar
        );
        editor.mMenuBar.draw();
        ImGui::End();
    }

    if (editor.isViewportMaximized()) {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, contentHeight));
        ImGui::Begin(
            "##MaximizedViewport",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );
        editor.mViewportPanel.draw(true);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin(
            "##StatusPanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );
        editor.mStatusPanel.draw();
        ImGui::End();
        return;
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
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
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
            ImVec2(curveX - 1, curveY),
            ImVec2(curveX - 1, curveY + curveH),
            IM_COL32(0x5a, 0x5a, 0x5a, 0xff)
        );
        ImGui::Begin(
            "##CurveEditorPanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );
        editor.mCurveEditorPanel.draw();
        ImGui::End();
    }

    float workspaceWidth = leftWidth - curveWidth;

    {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(workspaceWidth, viewportHeight));
        ImGui::Begin(
            "##ViewportPanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );
        editor.mViewportPanel.draw(false);
        ImGui::End();
    }

    {
        float timelineY = kMenuHeight + viewportHeight + kSplitterThickness;
        ImGui::SetNextWindowPos(ImVec2(0, timelineY));
        ImGui::SetNextWindowSize(ImVec2(workspaceWidth, timelineHeight));
        ImGui::Begin(
            "##TimelinePanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );
        editor.mTimelinePanel.draw();
        ImGui::End();
    }

    {
        Rect fullArea{
            {0.0f,          kMenuHeight                  },
            {displaySize.x, displaySize.y - kStatusHeight}
        };
        float splitterX = displaySize.x - detailsWidth - kSplitterThickness * 0.5f;
        ImGui::SetNextWindowPos(ImVec2(splitterX, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(kSplitterThickness, contentHeight));
        ImGui::Begin(
            "##DetailsSplitter",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
        );
        editor.mDetailsWidthRatio =
            editor.mSplitter.drawVerticalSplit(editor.mDetailsWidthRatio, fullArea, minDetailsRatio, maxDetailsRatio);
        ImGui::End();
    }

    {
        Rect leftArea{
            {0.0f,      kMenuHeight                  },
            {leftWidth, displaySize.y - kStatusHeight}
        };
        float splitterY = kMenuHeight + viewportHeight - kSplitterThickness * 0.5f;
        ImGui::SetNextWindowPos(ImVec2(0, splitterY));
        ImGui::SetNextWindowSize(ImVec2(leftWidth, kSplitterThickness));
        ImGui::Begin(
            "##TimelineSplitter",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
        );
        editor.mTimelineHeightRatio = editor.mSplitter.drawHorizontalSplit(
            1.0f - editor.mTimelineHeightRatio,
            leftArea,
            1.0f - maxTimelineRatio,
            1.0f - minTimelineRatio
        );
        editor.mTimelineHeightRatio        = 1.0f - editor.mTimelineHeightRatio;
        static float savedDetailsRatio     = editor.mDetailsWidthRatio;
        static float savedTimelineRatio    = editor.mTimelineHeightRatio;
        static float savedTrackListRatio   = editor.mTimelinePanel.trackListWidthRatio();
        static float savedZoomScale       = editor.mTimelinePanel.zoomScale();
        static float savedHorizontalScroll = editor.mTimelinePanel.horizontalScroll();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)
            && (savedDetailsRatio != editor.mDetailsWidthRatio || savedTimelineRatio != editor.mTimelineHeightRatio
                || savedTrackListRatio != editor.mTimelinePanel.trackListWidthRatio()
                || savedZoomScale != editor.mTimelinePanel.zoomScale()
                || savedHorizontalScroll != editor.mTimelinePanel.horizontalScroll())) {
            editor.saveLayoutPreferences();
            savedDetailsRatio     = editor.mDetailsWidthRatio;
            savedTimelineRatio    = editor.mTimelineHeightRatio;
            savedTrackListRatio   = editor.mTimelinePanel.trackListWidthRatio();
            savedZoomScale       = editor.mTimelinePanel.zoomScale();
            savedHorizontalScroll = editor.mTimelinePanel.horizontalScroll();
        }
        ImGui::End();
    }

    {
        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin(
            "##StatusPanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse
        );
        editor.mStatusPanel.draw();
        ImGui::End();
    }
}

} // namespace playback::editor::ui
