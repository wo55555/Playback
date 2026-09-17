#include "EditMode.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/ReplayEditor.h"

#include "imgui.h"

#include <algorithm>

namespace playback::editor::ui {

void EditMode::draw(PanelContext const& ctx) {
    auto& editor = ReplayEditor::getInstance();

    float const fontSize = ImGui::GetFontSize();
    auto const& style    = ImGui::GetStyle();
    // The menu bar is the only always-visible chrome, so it runs a size up from the rest of the editor.
    float const kMenuFontScale = 1.25f;
    float const kMenuHeight   = fontSize * kMenuFontScale + style.FramePadding.y * 2.0f + style.WindowBorderSize * 2.0f;
    float const kStatusHeight = metrics::statusBar();
    float const kSplitterThickness = metrics::splitter();
    float const kDetailsMinWidth   = std::max(240.0f, fontSize * 14.0f + metrics::rail());
    float const kViewportMinWidth  = std::max(320.0f, fontSize * 22.0f);
    float const kViewportMinHeight = std::max(180.0f, fontSize * 12.0f);
    float const kTimelineMinHeight =
        metrics::toolbarRow() + metrics::ruler() + metrics::cameraRow() * 2.0f + metrics::rangeBar();

    ImVec2 displaySize                    = ImGui::GetIO().DisplaySize;
    float  contentHeight                  = std::max(1.0f, displaySize.y - kMenuHeight - kStatusHeight);
    float  maxDetailsRatio                = std::min(0.50f, 1.0f - kViewportMinWidth / std::max(1.0f, displaySize.x));
    float  minDetailsRatio                = std::min(kDetailsMinWidth / std::max(1.0f, displaySize.x), maxDetailsRatio);
    editor.mDetailsWidthRatio             = std::clamp(editor.mDetailsWidthRatio, minDetailsRatio, maxDetailsRatio);
    float detailsWidth                    = displaySize.x * editor.mDetailsWidthRatio;
    float leftWidth                       = displaySize.x - detailsWidth;
    float maxTimelineRatio                = std::min(0.70f, 1.0f - kViewportMinHeight / contentHeight);
    float minTimelineRatio                = std::min(kTimelineMinHeight / contentHeight, maxTimelineRatio);
    editor.mTimelineHeightRatio           = std::clamp(editor.mTimelineHeightRatio, minTimelineRatio, maxTimelineRatio);
    float                  timelineHeight = contentHeight * editor.mTimelineHeightRatio;
    float                  viewportHeight = contentHeight - timelineHeight - kSplitterThickness;
    bool const             popupOpen      = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGuiWindowFlags const inputBlock     = popupOpen ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None;

    auto drawMenuBar = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kMenuHeight));
        ImGui::Begin(
            "##EditorMenuBar",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar
        );
        // Only the bar itself scales up; the dropdowns it opens inherit the editor's normal size.
        ImGui::SetWindowFontScale(kMenuFontScale);
        editor.mMenuBar.draw(ctx);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
    };

    // Panels paint their own toolbars and rails edge to edge, so their windows carry no padding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    if (editor.isViewportMaximized()) {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, contentHeight));
        ImGui::Begin(
            "##MaximizedViewport",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | inputBlock
        );
        editor.mViewportPanel.draw(ctx, true);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(0, displaySize.y - kStatusHeight));
        ImGui::SetNextWindowSize(ImVec2(displaySize.x, kStatusHeight));
        ImGui::Begin(
            "##StatusPanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | inputBlock
        );
        editor.mStatusPanel.draw(ctx);
        ImGui::End();
        ImGui::PopStyleVar();
        drawMenuBar();
        return;
    }

    {
        float detailsX = displaySize.x - detailsWidth;
        float detailsY = kMenuHeight;
        float detailsH = contentHeight;

        ImGui::SetNextWindowPos(ImVec2(detailsX, detailsY));
        ImGui::SetNextWindowSize(ImVec2(detailsWidth, detailsH));
        ImGui::Begin(
            "##DetailsPanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | inputBlock
        );
        editor.mDetailsPanel.draw(ctx);
        ImGui::End();
    }

    float workspaceWidth = leftWidth;

    {
        ImGui::SetNextWindowPos(ImVec2(0, kMenuHeight));
        ImGui::SetNextWindowSize(ImVec2(workspaceWidth, viewportHeight));
        ImGui::Begin(
            "##ViewportPanel",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar
                | ImGuiWindowFlags_NoScrollWithMouse | inputBlock
        );
        editor.mViewportPanel.draw(ctx, false);
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
                | ImGuiWindowFlags_NoScrollWithMouse | inputBlock
        );
        editor.mTimelinePanel.draw(ctx, !popupOpen);
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
                | inputBlock
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
                | inputBlock
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
        static float savedZoomScale        = editor.mTimelinePanel.zoomScale();
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
            savedZoomScale        = editor.mTimelinePanel.zoomScale();
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
                | ImGuiWindowFlags_NoScrollWithMouse | inputBlock
        );
        editor.mStatusPanel.draw(ctx);
        ImGui::End();
    }

    ImGui::PopStyleVar();
    drawMenuBar();
}

} // namespace playback::editor::ui
