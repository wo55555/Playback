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
    mTimelinePanel.setViewPreferences(0.30f, 0.25f, 0.0f);
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
            mTimelinePanel.setViewPreferences(trackListWidthRatio, 0.25f, 0.0f);

            auto const views = config.find("timelineViews");
            if (views != config.end() && views->is_object()) {
                for (auto const& [replayPath, view] : views->items()) {
                    if (replayPath.empty() || !view.is_object()) continue;
                    // Convert the legacy fit-relative zoom into the new absolute scale on first load.
                    mTimelineViewPreferences.insert_or_assign(
                        replayPath,
                        TimelineViewPreferences{
                            std::clamp(
                                readFiniteFloat(view, "pixelsPerTick", 0.25f * readFiniteFloat(view, "zoomScale", 1.0f)),
                                0.05f,
                                5.0f
                            ),
                            std::max(0.0f, readFiniteFloat(view, "horizontalScroll", 0.0f))
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
            {"pixelsPerTick",    preferences.pixelsPerTick   },
            {"horizontalScroll", preferences.horizontalScroll}
        };
    }
    if (!mActiveReplayPath.empty()) {
        timelineViews[mActiveReplayPath] = {
            {"pixelsPerTick",    mTimelinePanel.pixelsPerTick()   },
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

    if (!mActiveReplayPath.empty()) {
        mTimelineViewPreferences.insert_or_assign(
            mActiveReplayPath,
            TimelineViewPreferences{mTimelinePanel.pixelsPerTick(), mTimelinePanel.horizontalScroll()}
        );
    }

    mActiveReplayPath.assign(replayPath);
    auto const preferences = mTimelineViewPreferences.find(mActiveReplayPath);
    if (preferences == mTimelineViewPreferences.end()) {
        mTimelinePanel.setViewPreferences(mTimelinePanel.trackListWidthRatio(), 0.25f, 0.0f);
    } else {
        mTimelinePanel.setViewPreferences(
            mTimelinePanel.trackListWidthRatio(),
            preferences->second.pixelsPerTick,
            preferences->second.horizontalScroll
        );
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
    std::string_view replayPath;
    if (state.project) replayPath = state.project->projectPath;
    syncTimelineViewPreferences(replayPath);
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

    if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        submitAction({playback::editor::EditorActionType::TogglePause});
    }

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

    // Note: -/= are handled as separate checks since IsKeyPressed consumes the event
    if (ImGui::IsKeyPressed(ImGuiKey_Minus)) {
        submitAction({playback::editor::EditorActionType::DecreaseSpeed});
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal)) {
        submitAction({playback::editor::EditorActionType::IncreaseSpeed});
    }
}

} // namespace playback::editor::ui
