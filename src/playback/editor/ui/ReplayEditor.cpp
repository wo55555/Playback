#include "playback/editor/ui/ReplayEditor.h"

#include "playback/Playback.h"
#include "playback/editor/ui/ErrorDialog.h"
#include "playback/editor/ui/iconfont.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <utility>

namespace playback::editor::ui {

ReplayEditor& ReplayEditor::getInstance() {
    static ReplayEditor instance;
    return instance;
}

void ReplayEditor::initialize() {
    input::KeyMap::initialize();
    mModeManager.switchTo(EditorMode::Edit);
    loadLayoutPreferences();
}

void ReplayEditor::shutdown() {
    saveLayoutPreferences();
    mViewportMaximized = false;
    mCurveEditorPanel.setOpen(false);
    mFrameState = nullptr;
    mSubmit     = nullptr;
    mSelection.clear();
}

void ReplayEditor::setVideoAspectRatio(float aspectRatio) {
    mVideoAspectRatio = std::clamp(aspectRatio, 0.25f, 4.0f);
    mViewportPanel.setVideoAspectRatio(mVideoAspectRatio);
    saveLayoutPreferences();
}

void ReplayEditor::loadLayoutPreferences() {
    std::ifstream input("mods/playback/editor-layout.ini");
    float         detailsRatio{};
    float         timelineRatio{};
    float         aspectRatio{};
    float         trackListRatio{};
    float         pixelsPerTick{};
    float         horizontalScroll{};
    std::string   version;
    if (input >> version >> detailsRatio >> timelineRatio >> aspectRatio >> trackListRatio >> pixelsPerTick
            >> horizontalScroll
        && version == "v4" && std::isfinite(detailsRatio) && std::isfinite(timelineRatio) && std::isfinite(aspectRatio)
        && std::isfinite(trackListRatio) && std::isfinite(pixelsPerTick) && std::isfinite(horizontalScroll)) {
        mDetailsWidthRatio   = std::clamp(detailsRatio, 0.15f, 0.50f);
        mTimelineHeightRatio = std::clamp(timelineRatio, 0.18f, 0.65f);
        mVideoAspectRatio    = std::clamp(aspectRatio, 0.25f, 4.0f);
        mTimelinePanel.setViewPreferences(trackListRatio, pixelsPerTick, horizontalScroll);
    } else {
        input.clear();
        input.seekg(0);
        if (input >> detailsRatio >> timelineRatio >> aspectRatio && std::isfinite(detailsRatio)
            && std::isfinite(timelineRatio) && std::isfinite(aspectRatio)) {
            mDetailsWidthRatio   = std::clamp(detailsRatio, 0.15f, 0.50f);
            mTimelineHeightRatio = std::clamp(timelineRatio, 0.18f, 0.65f);
            mVideoAspectRatio    = std::clamp(aspectRatio, 0.25f, 4.0f);
        }
    }
    mViewportPanel.setVideoAspectRatio(mVideoAspectRatio);
}

void ReplayEditor::saveLayoutPreferences() const {
    std::ofstream output("mods/playback/editor-layout.ini", std::ios::trunc);
    if (output) {
        output << "v4 " << mDetailsWidthRatio << ' ' << mTimelineHeightRatio << ' ' << mVideoAspectRatio << ' '
               << mTimelinePanel.trackListWidthRatio() << ' ' << mTimelinePanel.pixelsPerTick() << ' '
               << mTimelinePanel.horizontalScroll();
    }
}

playback::editor::EditorState const& ReplayEditor::state() const {
    static playback::editor::EditorState const empty;
    return mFrameState ? *mFrameState : empty;
}

void ReplayEditor::submitAction(playback::editor::EditorAction action) const {
    if (mSubmit) (*mSubmit)(std::move(action));
}

void ReplayEditor::draw(playback::editor::EditorState const& state, SubmitAction const& submit) {
    if (!state.editorVisible) return;
    mFrameState = &state;
    mSubmit     = &submit;

    auto& io = ImGui::GetIO();
    float const savedFontScale = io.FontGlobalScale;
    io.FontGlobalScale = savedFontScale * (18.0f / 14.0f);
    mTheme.apply();

    if (!state.capabilities.videoExport && mModeManager.current() != EditorMode::Edit) {
        mModeManager.switchTo(EditorMode::Edit);
    }
    mEditMode.draw();

    ErrorDialog::getInstance().draw();
    handleKeyboardShortcuts();

    io.FontGlobalScale = savedFontScale;
    mFrameState = nullptr;
    mSubmit     = nullptr;
}

void ReplayEditor::handleKeyboardShortcuts() {
    ImGuiIO&    io           = ImGui::GetIO();
    auto const& currentState = state();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && mViewportMaximized) {
        mViewportMaximized = false;
        return;
    }
    if (io.WantTextInput) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        submitAction({playback::editor::EditorActionType::UndoEditorEdit});
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        submitAction({playback::editor::EditorActionType::RedoEditorEdit});
        return;
    }
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F)) {
        toggleViewportMaximized();
        return;
    }

    // ── Playback control ──
    if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        submitAction({playback::editor::EditorActionType::TogglePause});
    }

    // ── Seek ──
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        submitAction({playback::editor::EditorActionType::SkipToStart});
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        submitAction({playback::editor::EditorActionType::SkipToEnd});
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        int                            step = io.KeyShift ? 20 : 1;
        playback::editor::EditorAction action{playback::editor::EditorActionType::Seek};
        action.tick = std::max(0, currentState.currentTick - step);
        submitAction(std::move(action));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        int                            step = io.KeyShift ? 20 : 1;
        playback::editor::EditorAction action{playback::editor::EditorActionType::Seek};
        action.tick = std::min(currentState.totalTicks, currentState.currentTick + step);
        submitAction(std::move(action));
    }

    // ── Speed ──
    // Note: -/= are handled as separate checks since IsKeyPressed consumes the event
    if (ImGui::IsKeyPressed(ImGuiKey_Minus)) {
        submitAction({playback::editor::EditorActionType::DecreaseSpeed});
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal)) {
        submitAction({playback::editor::EditorActionType::IncreaseSpeed});
    }
}

} // namespace playback::editor::ui
