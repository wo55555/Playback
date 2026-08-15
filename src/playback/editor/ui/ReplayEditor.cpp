#include "playback/editor/ui/ReplayEditor.h"

#include "playback/Playback.h"
#include "playback/editor/ui/ErrorDialog.h"

#include "imgui.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <utility>

namespace playback::editor::ui {

namespace {

constexpr char kLayoutPreferencesPath[] = "mods/playback/editor-layout.json";

float readFiniteFloat(nlohmann::ordered_json const& object, char const* key, float fallback) {
    auto const value = object.find(key);
    if (value == object.end() || !value->is_number()) return fallback;
    float const result = value->get<float>();
    return std::isfinite(result) ? result : fallback;
}

} // namespace

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
    mTimelineViewPreferences.clear();
    mActiveReplayPath.clear();
    mViewportMaximized = false;
    mCurveEditorPanel.setOpen(false);
    mFrameState = nullptr;
    mSubmit     = nullptr;
    mSelection.clear();
    mLastExportState = exporting::ExportState::Idle;
}

void ReplayEditor::setVideoAspectRatio(float aspectRatio) {
    mVideoAspectRatio = std::clamp(aspectRatio, 0.25f, 4.0f);
    mViewportPanel.setVideoAspectRatio(mVideoAspectRatio);
    saveLayoutPreferences();
}

void ReplayEditor::loadLayoutPreferences() {
    mDetailsWidthRatio   = 0.28f;
    mTimelineHeightRatio = 0.35f;
    mVideoAspectRatio    = 16.0f / 9.0f;
    mTimelinePanel.setViewPreferences(0.30f, 1.0f, 0.0f);
    mTimelineViewPreferences.clear();
    mActiveReplayPath.clear();

    std::ifstream input(kLayoutPreferencesPath);
    if (input) {
        auto const config = nlohmann::ordered_json::parse(input, nullptr, false);
        if (config.is_object()) {
            mDetailsWidthRatio =
                std::clamp(readFiniteFloat(config, "detailsWidthRatio", mDetailsWidthRatio), 0.15f, 0.50f);
            mTimelineHeightRatio =
                std::clamp(readFiniteFloat(config, "timelineHeightRatio", mTimelineHeightRatio), 0.18f, 0.65f);
            mVideoAspectRatio = std::clamp(readFiniteFloat(config, "videoAspectRatio", mVideoAspectRatio), 0.25f, 4.0f);
            float const trackListWidthRatio =
                readFiniteFloat(config, "trackListWidthRatio", mTimelinePanel.trackListWidthRatio());
            mTimelinePanel.setViewPreferences(trackListWidthRatio, 1.0f, 0.0f);

            auto const views = config.find("timelineViews");
            if (views != config.end() && views->is_object()) {
                for (auto const& [replayPath, view] : views->items()) {
                    if (replayPath.empty() || !view.is_object()) continue;
                    auto const zoomScale = view.find("zoomScale");
                    mTimelineViewPreferences.insert_or_assign(
                        replayPath,
                        TimelineViewPreferences{
                            zoomScale == view.end() ? 1.0f
                                                    : std::clamp(readFiniteFloat(view, "zoomScale", 1.0f), 1.0f, 20.0f),
                            zoomScale == view.end() ? 0.0f
                                                    : std::max(0.0f, readFiniteFloat(view, "horizontalScroll", 0.0f))
                        }
                    );
                }
            }
        }
    }
    mViewportPanel.setVideoAspectRatio(mVideoAspectRatio);
}

void ReplayEditor::saveLayoutPreferences() const {
    nlohmann::ordered_json timelineViews = nlohmann::ordered_json::object();
    for (auto const& [replayPath, preferences] : mTimelineViewPreferences) {
        if (replayPath == mActiveReplayPath) continue;
        timelineViews[replayPath] = {
            {"zoomScale",        preferences.zoomScale       },
            {"horizontalScroll", preferences.horizontalScroll}
        };
    }
    if (!mActiveReplayPath.empty()) {
        timelineViews[mActiveReplayPath] = {
            {"zoomScale",        mTimelinePanel.zoomScale()       },
            {"horizontalScroll", mTimelinePanel.horizontalScroll()}
        };
    }

    nlohmann::ordered_json const config{
        {"detailsWidthRatio",   mDetailsWidthRatio                  },
        {"timelineHeightRatio", mTimelineHeightRatio                },
        {"videoAspectRatio",    mVideoAspectRatio                   },
        {"trackListWidthRatio", mTimelinePanel.trackListWidthRatio()},
        {"timelineViews",       std::move(timelineViews)            }
    };
    std::ofstream output(kLayoutPreferencesPath, std::ios::trunc);
    if (output) {
        output << config.dump(2);
    }
}

void ReplayEditor::syncTimelineViewPreferences(std::string_view replayPath) {
    if (mActiveReplayPath == replayPath) return;
    bool const enteringReplay = mActiveReplayPath.empty() && !replayPath.empty();

    if (!mActiveReplayPath.empty()) {
        mTimelineViewPreferences.insert_or_assign(
            mActiveReplayPath,
            TimelineViewPreferences{mTimelinePanel.zoomScale(), mTimelinePanel.horizontalScroll()}
        );
    }

    mActiveReplayPath.assign(replayPath);
    auto const preferences = mTimelineViewPreferences.find(mActiveReplayPath);
    if (enteringReplay || preferences == mTimelineViewPreferences.end()) {
        mTimelinePanel.setViewPreferences(mTimelinePanel.trackListWidthRatio(), 1.0f, 0.0f);
    } else {
        mTimelinePanel.setViewPreferences(
            mTimelinePanel.trackListWidthRatio(),
            preferences->second.zoomScale,
            preferences->second.horizontalScroll
        );
    }
}

playback::state::EditorState const& ReplayEditor::state() const {
    static playback::state::EditorState const empty;
    return mFrameState ? *mFrameState : empty;
}

void ReplayEditor::submitAction(playback::state::EditorAction action) const {
    if (mSubmit) (*mSubmit)(std::move(action));
}

void ReplayEditor::openExportDialog() {
    auto const& currentState = state();
    if (!currentState.capabilities.videoExport || currentState.totalTicks <= 0
        || exporting::isExportActive(currentState.exportStatus.state)) {
        return;
    }
    mMenuBar.openExportDialog(currentState.totalTicks, currentState.capabilities.ffmpegVideoExport);
}

void ReplayEditor::seekTo(int tick) { mTimelinePanel.seekTo(tick); }

void ReplayEditor::seekRelative(int tickDelta) { mTimelinePanel.seekRelative(tickDelta); }

bool ReplayEditor::deleteSelection() { return mTimelinePanel.deleteSelection(); }

bool ReplayEditor::addKeyframeAtPlayhead() { return mTimelinePanel.addKeyframeAtPlayhead(); }

bool ReplayEditor::splitAtPlayhead() { return mTimelinePanel.splitAtPlayhead(); }

void ReplayEditor::draw(playback::state::EditorState const& state, SubmitAction const& submit) {
    if (!state.editorVisible) {
        if (!mActiveReplayPath.empty()) {
            mTimelineViewPreferences.insert_or_assign(
                mActiveReplayPath,
                TimelineViewPreferences{mTimelinePanel.zoomScale(), mTimelinePanel.horizontalScroll()}
            );
            mActiveReplayPath.clear();
        }
        return;
    }
    std::string_view replayPath;
    if (state.project) replayPath = state.project->projectPath;
    syncTimelineViewPreferences(replayPath);
    mFrameState = &state;
    mSubmit     = &submit;

    auto&       io             = ImGui::GetIO();
    float const savedFontScale = io.FontGlobalScale;
    io.FontGlobalScale         = savedFontScale * (18.0f / 14.0f);
    mTheme.apply();

    auto const exportActive = exporting::isExportActive(state.exportStatus.state);
    if (exportActive && mModeManager.current() != EditorMode::Render) {
        mModeManager.switchTo(EditorMode::Render);
    } else if (!exportActive && mModeManager.current() != EditorMode::Edit) {
        mModeManager.switchTo(EditorMode::Edit);
    }
    if (state.exportStatus.state == exporting::ExportState::Faulted
        && mLastExportState != exporting::ExportState::Faulted) {
        ErrorDialog::getInstance().show(std::string_view{}, state.exportStatus.message);
    }
    mLastExportState = state.exportStatus.state;

    if (mModeManager.current() == EditorMode::Render) mRenderMode.draw();
    else mEditMode.draw();

    ErrorDialog::getInstance().draw();
    if (!exportActive) handleKeyboardShortcuts();

    io.FontGlobalScale = savedFontScale;
    mFrameState        = nullptr;
    mSubmit            = nullptr;
}

void ReplayEditor::handleKeyboardShortcuts() {
    using input::EditorKeybind;

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        return;
    }

    if (mViewportMaximized && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        mViewportMaximized = false;
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::Undo)) {
        submitAction({playback::state::EditorActionType::UndoEditorEdit});
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::Redo)) {
        submitAction({playback::state::EditorActionType::RedoEditorEdit});
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ToggleViewportMaximized)) {
        toggleViewportMaximized();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::OpenExport)) {
        openExportDialog();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::DeleteSelection)) {
        (void)deleteSelection();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::AddKeyframe)) {
        (void)addKeyframeAtPlayhead();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SplitAtPlayhead)) {
        (void)splitAtPlayhead();
        return;
    }

    if (input::KeyMap::pressed(EditorKeybind::JumpStart)) {
        seekTo(0);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::JumpEnd)) {
        seekTo(state().totalTicks);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekTickLeft, true)) {
        mTimelinePanel.seekRelative(-1);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekTickRight, true)) {
        mTimelinePanel.seekRelative(1);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekSecondLeft, true)) {
        mTimelinePanel.seekRelative(-20);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekSecondRight, true)) {
        mTimelinePanel.seekRelative(20);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::PreviousEditPoint, true)) {
        mTimelinePanel.seekAdjacentEditPoint(false);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::NextEditPoint, true)) {
        mTimelinePanel.seekAdjacentEditPoint(true);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ZoomOutTimeline, true)) {
        mTimelinePanel.zoomOut();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ZoomInTimeline, true)) {
        mTimelinePanel.zoomIn();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ResetTimelineZoom)) {
        mTimelinePanel.resetZoom();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::DecreaseSpeed, true)) {
        submitAction({playback::state::EditorActionType::DecreaseSpeed});
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::IncreaseSpeed, true)) {
        submitAction({playback::state::EditorActionType::IncreaseSpeed});
    }
}

} // namespace playback::editor::ui
