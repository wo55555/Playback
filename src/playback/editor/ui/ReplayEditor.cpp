#include "playback/editor/ui/ReplayEditor.h"

#include "playback/Playback.h"
#include "playback/audio/PreviewAudio.h"
#include "playback/editor/input/EditorInput.h"
#include "playback/editor/input/KeyMap.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/ErrorDialog.h"
#include "playback/editor/ui/components/HistoryLabel.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/state/editing/ClipOps.h"
#include "playback/state/editing/TimelineNavigation.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <ranges>
#include <utility>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

constexpr char  kLayoutPreferencesPath[] = "mods/playback/editor-layout.json";
constexpr float kExitHoldDuration        = 0.8f;
constexpr float kExitHoldFeedbackDelay   = 0.12f;

float readFiniteFloat(nlohmann::ordered_json const& object, char const* key, float fallback) {
    auto const value = object.find(key);
    if (value == object.end() || !value->is_number()) return fallback;
    float const result = value->get<float>();
    return std::isfinite(result) ? result : fallback;
}

bool readBool(nlohmann::ordered_json const& object, char const* key, bool fallback) {
    auto const value = object.find(key);
    return value != object.end() && value->is_boolean() ? value->get<bool>() : fallback;
}

struct HudPreferenceKey {
    visuals::HudElement element;
    char const*         key;
};
constexpr std::array<HudPreferenceKey, visuals::HudElementCount> kHudPreferenceKeys{
    {
     {visuals::HudElement::Chat, "chat"},
     {visuals::HudElement::BossBar, "bossBar"},
     {visuals::HudElement::Title, "title"},
     {visuals::HudElement::Scoreboard, "scoreboard"},
     {visuals::HudElement::ActionBar, "actionBar"},
     {visuals::HudElement::Hotbar, "hotbar"},
     }
};

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
    mViewportPanel.clearCameraPath();
    mFrameState = nullptr;
    mSubmit     = nullptr;
    mSelection.clear();
    mLastExportState = exporting::ExportState::Idle;
    mToast.clear();
    mLastHistoryEventSeq = 0;
    mExitHoldSeconds     = 0.0f;
    mExitHoldBlocked = false;
}

void ReplayEditor::setVideoAspectRatio(float aspectRatio) {
    mVideoAspectRatio = std::clamp(aspectRatio, 0.25f, 4.0f);
    mViewportPanel.setVideoAspectRatio(mVideoAspectRatio);
    saveLayoutPreferences();
}

void ReplayEditor::toggleCameraPath() {
    mCameraPathVisible = !mCameraPathVisible;
    saveLayoutPreferences();
}

void ReplayEditor::setInfoOverlayVisible(bool visible) {
    mViewportPanel.setInfoOverlayVisible(visible);
    saveLayoutPreferences();
}

void ReplayEditor::setAutoPreviewEnabled(bool enabled) {
    mViewportPanel.setAutoPreviewEnabled(enabled);
    saveLayoutPreferences();
}

void ReplayEditor::setSnapEnabled(bool enabled) {
    mTimelinePanel.setSnapEnabled(enabled);
    saveLayoutPreferences();
}

void ReplayEditor::setUiScaleTier(UiScaleTier tier) {
    mUiScaleTier = tier;
    setCurrentUiScaleTier(tier);
    saveLayoutPreferences();
}

bool ReplayEditor::isHudElementShown(visuals::HudElement element) const { return visuals::isHudElementShown(element); }

void ReplayEditor::setHudElementShown(visuals::HudElement element, bool shown) {
    visuals::setHudElementShown(element, shown);
    saveLayoutPreferences();
}

bool ReplayEditor::isPreviewAudioMuted() const { return audio::preview::isMuted(); }

void ReplayEditor::setPreviewAudioMuted(bool muted) {
    audio::preview::setMuted(muted);
    saveLayoutPreferences();
}

float ReplayEditor::previewAudioVolume() const { return audio::preview::volume(); }

void ReplayEditor::setPreviewAudioVolume(float volume, bool commit) {
    audio::preview::setVolume(volume);
    if (commit) saveLayoutPreferences();
}

bool ReplayEditor::isPreviewSilentWhilePaused() const { return audio::preview::isSilentWhilePaused(); }

void ReplayEditor::setPreviewSilentWhilePaused(bool silent) {
    audio::preview::setSilentWhilePaused(silent);
    saveLayoutPreferences();
}

void ReplayEditor::loadLayoutPreferences() {
    mDetailsWidthRatio   = 0.22f;
    mTimelineHeightRatio = 0.24f;
    mVideoAspectRatio    = 16.0f / 9.0f;
    mCameraPathVisible   = true;
    mUiScaleTier         = UiScaleTier::Auto;
    mViewportPanel.setInfoOverlayVisible(true);
    mViewportPanel.setAutoPreviewEnabled(true);
    mTimelinePanel.setSnapEnabled(true);
    mTimelinePanel.setViewPreferences(0.18f, 1.0f, 0.0f);
    mTimelineViewPreferences.clear();
    mActiveReplayPath.clear();

    std::ifstream input(kLayoutPreferencesPath);
    if (input) {
        auto const config = nlohmann::ordered_json::parse(input, nullptr, false);
        if (config.is_object()) {
            auto const cameraPath = config.find("showCameraPath");
            if (cameraPath != config.end() && cameraPath->is_boolean()) mCameraPathVisible = cameraPath->get<bool>();
            mViewportPanel.setInfoOverlayVisible(
                readBool(config, "infoOverlay", mViewportPanel.isInfoOverlayVisible())
            );
            mViewportPanel.setAutoPreviewEnabled(
                readBool(config, "autoPreview", mViewportPanel.isAutoPreviewEnabled())
            );
            mTimelinePanel.setSnapEnabled(readBool(config, "snap", mTimelinePanel.isSnapEnabled()));
            auto const uiScale = config.find("uiScale");
            if (uiScale != config.end() && uiScale->is_string()) {
                mUiScaleTier = uiScaleTierFromName(uiScale->get<std::string>());
            }
            mDetailsWidthRatio =
                std::clamp(readFiniteFloat(config, "detailsWidthRatio", mDetailsWidthRatio), 0.15f, 0.50f);
            mTimelineHeightRatio =
                std::clamp(readFiniteFloat(config, "timelineHeightRatio", mTimelineHeightRatio), 0.15f, 0.65f);
            mVideoAspectRatio = std::clamp(readFiniteFloat(config, "videoAspectRatio", mVideoAspectRatio), 0.25f, 4.0f);
            float const trackListWidthRatio =
                readFiniteFloat(config, "trackListWidthRatio", mTimelinePanel.trackListWidthRatio());
            mTimelinePanel.setViewPreferences(trackListWidthRatio, 1.0f, 0.0f);
            auto& replayConfig = Playback::getInstance().getConfig().replay;
            replayConfig.disableServerResourcePacks =
                readBool(config, "disableServerResourcePacks", replayConfig.disableServerResourcePacks);

            auto const hud = config.find("hud");
            if (hud != config.end() && hud->is_object()) {
                for (auto const& entry : kHudPreferenceKeys) {
                    visuals::setHudElementShown(
                        entry.element,
                        readBool(*hud, entry.key, visuals::isHudElementShown(entry.element))
                    );
                }
            }
            auto const previewAudio = config.find("previewAudio");
            if (previewAudio != config.end() && previewAudio->is_object()) {
                audio::preview::setMuted(readBool(*previewAudio, "muted", audio::preview::isMuted()));
                audio::preview::setVolume(readFiniteFloat(*previewAudio, "volume", audio::preview::volume()));
                audio::preview::setSilentWhilePaused(
                    readBool(*previewAudio, "silentWhilePaused", audio::preview::isSilentWhilePaused())
                );
            }

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
    setCurrentUiScaleTier(mUiScaleTier);
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

    nlohmann::ordered_json hud = nlohmann::ordered_json::object();
    for (auto const& entry : kHudPreferenceKeys) hud[entry.key] = visuals::isHudElementShown(entry.element);
    nlohmann::ordered_json previewAudio{
        {"muted",             audio::preview::isMuted()            },
        {"volume",            audio::preview::volume()             },
        {"silentWhilePaused", audio::preview::isSilentWhilePaused()}
    };

    nlohmann::ordered_json const config{
        {"detailsWidthRatio",          mDetailsWidthRatio                                                   },
        {"timelineHeightRatio",        mTimelineHeightRatio                                                 },
        {"videoAspectRatio",           mVideoAspectRatio                                                    },
        {"showCameraPath",             mCameraPathVisible                                                   },
        {"infoOverlay",                mViewportPanel.isInfoOverlayVisible()                                },
        {"autoPreview",                mViewportPanel.isAutoPreviewEnabled()                                },
        {"snap",                       mTimelinePanel.isSnapEnabled()                                       },
        {"uiScale",                    std::string(uiScaleTierName(mUiScaleTier))                           },
        {"trackListWidthRatio",        mTimelinePanel.trackListWidthRatio()                                 },
        {"disableServerResourcePacks", Playback::getInstance().getConfig().replay.disableServerResourcePacks},
        {"hud",                        std::move(hud)                                                       },
        {"previewAudio",               std::move(previewAudio)                                              },
        {"timelineViews",              std::move(timelineViews)                                             }
    };
    std::ofstream output(kLayoutPreferencesPath, std::ios::trunc);
    if (output) {
        output << config.dump(2);
    }
}

void ReplayEditor::syncTimelineViewPreferences(std::string_view replayPath) {
    if (mActiveReplayPath == replayPath) return;
    mViewportPanel.clearCameraPath();
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
    mMenuBar.openExportDialog(
        currentState.totalTicks,
        currentState.capabilities.ffmpegVideoExport,
        exportStartTick(),
        exportEndTick()
    );
}

void ReplayEditor::openExportSingleClip(std::string const& clipId) {
    auto const& currentState = state();
    if (!currentState.capabilities.videoExport || currentState.totalTicks <= 0
        || exporting::isExportActive(currentState.exportStatus.state)) {
        return;
    }
    mMenuBar.openExportSingleClip(
        clipId,
        currentState.totalTicks,
        currentState.capabilities.ffmpegVideoExport,
        exportStartTick(),
        exportEndTick()
    );
}

void ReplayEditor::openExportScene(std::string const& sceneId) {
    auto const& currentState = state();
    if (!currentState.capabilities.videoExport || currentState.totalTicks <= 0
        || exporting::isExportActive(currentState.exportStatus.state)) {
        return;
    }
    std::string const activeSceneId = currentState.project ? currentState.project->activeSceneId : std::string{};
    size_t const      sceneCount    = currentState.project ? currentState.project->scenes.size() : size_t{0};
    mMenuBar.openExportScene(
        sceneId,
        activeSceneId,
        sceneCount,
        currentState.totalTicks,
        currentState.capabilities.ffmpegVideoExport,
        exportStartTick(),
        exportEndTick()
    );
}

int ReplayEditor::exportStartTick() const { return state().project ? state().project->exportInTick : -1; }

int ReplayEditor::exportEndTick() const { return state().project ? state().project->exportOutTick : -1; }

void ReplayEditor::setExportRange(int inTick, int outTick) {
    playback::state::EditorAction action{playback::state::EditorActionType::SetSceneExportRange};
    action.tick          = inTick;
    action.secondaryTick = outTick;
    submitAction(std::move(action));
}

void ReplayEditor::markExportPoint(bool isIn) {
    int const totalTicks = std::max(0, state().totalTicks);
    if (totalTicks <= 0) return;
    int const tick  = std::clamp(state().currentTick, 0, totalTicks);
    int       start = exportStartTick() < 0 ? 0 : std::clamp(exportStartTick(), 0, totalTicks);
    int       end   = exportEndTick() < 0 ? totalTicks : std::clamp(exportEndTick(), 0, totalTicks);
    if (isIn) {
        if (tick >= end) end = totalTicks;
        start = tick;
    } else {
        if (tick <= start) start = 0;
        end = tick;
    }
    if (start >= end || (start <= 0 && end >= totalTicks)) {
        setExportRange(-1, -1);
    } else {
        setExportRange(start, end);
    }
}

void ReplayEditor::clearExportRange() { setExportRange(-1, -1); }

PanelContext ReplayEditor::frameContext() {
    static SubmitAction const noSubmit{};
    return {state(), mSelection, mSubmit ? *mSubmit : noSubmit, *this, graphics::currentRenderCameraProjection()};
}

void ReplayEditor::seekTo(int tick) { mTimelinePanel.seekTo(frameContext(), tick); }

void ReplayEditor::seekRelative(int tickDelta) { mTimelinePanel.seekRelative(frameContext(), tickDelta); }

void ReplayEditor::undo(std::size_t steps) {
    playback::state::EditorAction action{playback::state::EditorActionType::UndoEditorEdit};
    action.historySteps = std::max<std::size_t>(1, steps);
    submitAction(std::move(action));
}

void ReplayEditor::redo(std::size_t steps) {
    playback::state::EditorAction action{playback::state::EditorActionType::RedoEditorEdit};
    action.historySteps = std::max<std::size_t>(1, steps);
    submitAction(std::move(action));
}

bool ReplayEditor::deleteSelection() { return mTimelinePanel.deleteSelection(frameContext()); }

bool ReplayEditor::addKeyframeAtPlayhead() { return mTimelinePanel.addKeyframeAtPlayhead(frameContext()); }

bool ReplayEditor::selectAllKeyframes() { return mTimelinePanel.selectAllKeyframes(frameContext()); }

bool ReplayEditor::copySelection() { return mTimelinePanel.copySelection(frameContext()); }

bool ReplayEditor::pasteAtPlayhead() { return mTimelinePanel.pasteAtPlayhead(frameContext()); }

bool ReplayEditor::addMarkerAtPlayhead() { return mTimelinePanel.addMarkerAtPlayhead(frameContext()); }

bool ReplayEditor::canSplitClipAtPlayhead() { return mTimelinePanel.canSplitClipAtPlayhead(frameContext()); }

bool ReplayEditor::splitClipAtPlayhead() { return mTimelinePanel.splitClipAtPlayhead(frameContext()); }

bool ReplayEditor::canTrimClipAtPlayhead(bool trimIn) {
    return mTimelinePanel.canTrimClipAtPlayhead(frameContext(), trimIn);
}

bool ReplayEditor::trimClipAtPlayhead(bool trimIn) { return mTimelinePanel.trimClipAtPlayhead(frameContext(), trimIn); }

bool ReplayEditor::toggleSelectedClipExport() {
    auto const project = state().project;
    if (!project) return false;
    int const tick       = state().currentTick;
    int const totalTicks = state().totalTicks;

    state::editing::model::Clip const* clip = nullptr;
    if (auto const* selectedClip = mSelection.getAs<state::editing::model::SelectedClip>()) {
        if (auto const hit = state::editing::ClipOps::findClipWithCamera(*project, selectedClip->clipId);
            hit && !hit.camera->locked) {
            clip = hit.clip;
        }
    } else {
        std::string selectedCameraId;
        if (auto const* selectedCamera = mSelection.getAs<state::editing::model::SelectedCamera>()) {
            selectedCameraId = selectedCamera->cameraId;
        } else if (auto const* selectedKeyframe = mSelection.getAs<state::editing::model::SelectedKeyframe>()) {
            selectedCameraId = selectedKeyframe->trackId;
        }
        if (!selectedCameraId.empty()) {
            auto const camIt =
                std::ranges::find(project->cameras, selectedCameraId, &state::editing::model::CameraEntity::id);
            if (camIt != project->cameras.end() && !camIt->locked) {
                for (auto const& candidate : camIt->clips) {
                    if (state::editing::ClipOps::clipCoversTick(candidate, tick, totalTicks)) {
                        clip = &candidate;
                        break;
                    }
                }
            }
        } else {
            static std::vector<int> const emptyTransitions;
            auto const&                   transitions =
                state().cameraTimeline ? state().cameraTimeline->dimensionTransitionTicks() : emptyTransitions;
            if (auto const active = state::editing::ClipOps::activeClipAt(*project, tick, false, transitions);
                active && !active.camera->locked) {
                clip = active.clip;
            } else {
                for (auto const& camera : project->cameras) {
                    if (camera.locked) continue;
                    for (auto const& candidate : camera.clips) {
                        if (state::editing::ClipOps::clipCoversTick(candidate, tick, totalTicks)) {
                            clip = &candidate;
                            break;
                        }
                    }
                    if (clip) break;
                }
            }
        }
    }
    if (!clip) return false;
    playback::state::EditorAction action{playback::state::EditorActionType::SetClipExportEnabled};
    action.id    = clip->id;
    action.value = !clip->exportEnabled;
    submitAction(std::move(action));
    return true;
}

bool ReplayEditor::selectCameraByIndex(std::size_t index) {
    auto const project = state().project;
    if (!project || index >= project->cameras.size()) return false;

    auto const& camera = project->cameras[index];
    mSelection.select(state::editing::model::SelectedCamera{camera.id});
    // Matches the viewport dropdown: selecting a camera also previews through it.
    playback::state::EditorAction action{playback::state::EditorActionType::SetPreviewCamera};
    action.id = camera.id;
    submitAction(std::move(action));
    return true;
}

void ReplayEditor::draw(playback::state::EditorState const& state, SubmitAction const& submit) {
    bool const exportActive = exporting::isExportActive(state.exportStatus.state);
    if (!state.editorVisible && !exportActive) {
        mToast.clear();
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
    // A reloaded or edited project can drop the selected object; without this the inspector shows "missing".
    if (state.project) mSelection.pruneInvalid(*state.project);

    theme::apply();

    if (exportActive && mModeManager.current() != EditorMode::Render) {
        mModeManager.switchTo(EditorMode::Render);
        mRenderMode.reset();
    } else if (!exportActive && mModeManager.current() != EditorMode::Edit) {
        mModeManager.switchTo(EditorMode::Edit);
    }
    if (state.exportStatus.state == exporting::ExportState::Faulted
        && mLastExportState != exporting::ExportState::Faulted) {
        ErrorDialog::getInstance().show(std::string_view{}, state.exportStatus.message);
    }
    mLastExportState = state.exportStatus.state;

    bool const escapeConsumedByUi = !exportActive
                                 && (mViewportMaximized || ImGui::GetIO().WantTextInput
                                     || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));

    if (state.historyEvent.seq != mLastHistoryEventSeq) {
        mLastHistoryEventSeq = state.historyEvent.seq;
        if (state.historyEvent.kind != playback::state::EditorHistoryEventKind::None && !exportActive
            && state.editorVisible) {
            auto const  labelText = translateHistoryLabel(state.historyEvent.label);
            std::string message;
            if (state.historyEvent.kind == playback::state::EditorHistoryEventKind::Undo) {
                message = state.historyEvent.steps > 1
                            ? "playback.refactorEditor.toast.undoSteps"_tr(state.historyEvent.steps, labelText)
                            : "playback.refactorEditor.toast.undo"_tr(labelText);
            } else if (state.historyEvent.kind == playback::state::EditorHistoryEventKind::Redo) {
                message = state.historyEvent.steps > 1
                            ? "playback.refactorEditor.toast.redoSteps"_tr(state.historyEvent.steps, labelText)
                            : "playback.refactorEditor.toast.redo"_tr(labelText);
            }
            if (!message.empty()) mToast.show(std::move(message));
        }
    }

    auto const ctx = frameContext();
    if (mModeManager.current() == EditorMode::Render) mRenderMode.draw(ctx);
    else mEditMode.draw(ctx);

    if (exportActive || !state.editorVisible) {
        mToast.clear();
    } else {
        ImVec2     toastPos{};
        ImVec2     toastSize{};
        Rect const vpRect = mViewportPanel.videoRect();
        if (vpRect.GetWidth() > 0.0f && vpRect.GetHeight() > 0.0f) {
            toastPos           = vpRect.min;
            toastSize          = {vpRect.GetWidth(), vpRect.GetHeight()};
            Rect const overlay = mViewportPanel.overlayRect();
            if (mViewportMaximized && overlay.GetHeight() > 0.0f && overlay.min.y > toastPos.y) {
                toastSize.y = std::min(toastSize.y, overlay.min.y - toastPos.y);
            }
        }
        mToast.draw(toastPos, toastSize, ImGui::GetIO().DeltaTime);
    }

    ErrorDialog::getInstance().draw();
    updateExitHold(escapeConsumedByUi);
    if (!exportActive) {
        handleKeyboardShortcuts();
        updateFollowExitTap();
    }

    mFrameState = nullptr;
    mSubmit     = nullptr;
}

// `escapeConsumedByUi` is sampled before the panels draw: a press that closes a popup, leaves a text field or
// restores the maximized viewport is already spent and must not also count towards leaving the editor.
void ReplayEditor::updateExitHold(bool escapeConsumedByUi) {
    ImGuiIO const& io = ImGui::GetIO();
    if (!ImGui::IsKeyDown(ImGuiKey_Escape)) {
        mExitHoldSeconds = 0.0f;
        mExitHoldBlocked = false;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) mExitHoldBlocked = escapeConsumedByUi;
    if (mExitHoldBlocked) {
        mExitHoldSeconds = 0.0f;
        return;
    }

    float const previous  = mExitHoldSeconds;
    mExitHoldSeconds     += io.DeltaTime;
    if (previous < kExitHoldDuration && mExitHoldSeconds >= kExitHoldDuration) {
        submitAction({playback::state::EditorActionType::StopReplay});
        return;
    }
    if (mExitHoldSeconds < kExitHoldFeedbackDelay) return;

    float const  progress = std::clamp(mExitHoldSeconds / kExitHoldDuration, 0.0f, 1.0f);
    auto const   label    = "playback.refactorEditor.menu.exitHoldPrompt"_tr();
    float const  pad      = 10.0f * metrics::scale();
    ImVec2 const textSize = ImGui::CalcTextSize(label.c_str());
    ImVec2 const size{textSize.x + pad * 2.0f, textSize.y + pad * 2.0f};
    ImVec2 const origin{
        ImGui::GetMainViewport()->Pos.x + (ImGui::GetMainViewport()->Size.x - size.x) * 0.5f,
        ImGui::GetMainViewport()->Pos.y + metrics::font() * 3.0f
    };
    float const barHeight = std::max(3.0f, 3.0f * metrics::scale());

    auto* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, theme::kOverlayBg, size.y * 0.5f);
    drawList->AddText({origin.x + pad, origin.y + pad}, theme::kText, label.c_str());
    ImVec2 const barMin{origin.x + size.y * 0.5f, origin.y + size.y - barHeight - 3.0f};
    float const  barWidth = size.x - size.y;
    drawList->AddRectFilled(barMin, {barMin.x + barWidth, barMin.y + barHeight}, theme::kRangeTrack, barHeight * 0.5f);
    drawList->AddRectFilled(
        barMin,
        {barMin.x + barWidth * progress, barMin.y + barHeight},
        theme::kWarning,
        barHeight * 0.5f
    );
}

// A lone Shift tap leaves spectating, so Shift+click and Ctrl+Shift chords stay free.
void ReplayEditor::updateFollowExitTap() {
    ImGuiIO const& io        = ImGui::GetIO();
    bool const     shiftDown = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    if (!shiftDown) {
        bool const tapped   = mFollowExitTapArmed;
        mFollowExitTapArmed = false;
        if (!tapped || !state().followingRecordedPlayer || input::isGameInputCaptured() || io.WantTextInput
            || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            return;
        }
        playback::state::EditorAction action{playback::state::EditorActionType::SetFollowRecordedPlayer};
        action.value = false;
        submitAction(std::move(action));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftShift, false) || ImGui::IsKeyPressed(ImGuiKey_RightShift, false)) {
        mFollowExitTapArmed = !io.KeyCtrl && !io.KeyAlt && !ImGui::IsAnyMouseDown();
        return;
    }
    if (!mFollowExitTapArmed) return;
    if (io.KeyCtrl || io.KeyAlt || ImGui::IsAnyMouseDown() || io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f) {
        mFollowExitTapArmed = false;
        return;
    }
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_ReservedForModCtrl; ++key) {
        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) {
            mFollowExitTapArmed = false;
            return;
        }
    }
}

void ReplayEditor::handleKeyboardShortcuts() {
    using input::EditorKeybind;

    // While the camera is being driven the game owns the keyboard; only the exit hold stays live.
    if (input::isGameInputCaptured()) return;

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        return;
    }

    if (mViewportMaximized && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        mViewportMaximized = false;
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ClearSelection)) {
        mSelection.clear();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SelectAll)) {
        (void)selectAllKeyframes();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::Copy)) {
        (void)copySelection();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::Paste)) {
        (void)pasteAtPlayhead();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::InsertMarker)) {
        (void)addMarkerAtPlayhead();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SelectTool)) {
        setTimelineTool(TimelineTool::Select);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::RazorTool)) {
        setTimelineTool(TimelineTool::Razor);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::PenTool)) {
        setTimelineTool(TimelineTool::Pen);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SplitClip)) {
        (void)splitClipAtPlayhead();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ToggleClipExport)) {
        (void)toggleSelectedClipExport();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::TrimClipInToPlayhead)) {
        (void)trimClipAtPlayhead(true);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::TrimClipOutToPlayhead)) {
        (void)trimClipAtPlayhead(false);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ClearExportRange)) {
        clearExportRange();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::PreviousMarker, true)) {
        mTimelinePanel.seekAdjacentMarker(frameContext(), false);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::NextMarker, true)) {
        mTimelinePanel.seekAdjacentMarker(frameContext(), true);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SaveProject)) {
        submitAction({playback::state::EditorActionType::SaveProject});
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::Undo, true)) {
        undo();
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::Redo, true)) {
        redo();
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
    if (input::KeyMap::pressed(EditorKeybind::AddCameraTrack)) {
        submitAction({playback::state::EditorActionType::AddFreeCamera});
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ToggleFollowPerspective)) {
        if (state().followingRecordedPlayer) {
            playback::state::EditorAction action{playback::state::EditorActionType::SetFollowThirdPerson};
            action.value = !state().followThirdPerson;
            submitAction(std::move(action));
        }
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SelectCamera1)) {
        (void)selectCameraByIndex(0);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SelectCamera2)) {
        (void)selectCameraByIndex(1);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SelectCamera3)) {
        (void)selectCameraByIndex(2);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::JumpStart)) {
        mTimelinePanel.seekSmartEdge(frameContext(), false);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::JumpEnd)) {
        mTimelinePanel.seekSmartEdge(frameContext(), true);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekTickLeft, true)) {
        mTimelinePanel.seekRelative(frameContext(), -1);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekTickRight, true)) {
        mTimelinePanel.seekRelative(frameContext(), 1);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekSecondLeft, true)) {
        mTimelinePanel.seekRelative(frameContext(), -20);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::SeekSecondRight, true)) {
        mTimelinePanel.seekRelative(frameContext(), 20);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::PreviousEditPoint, true)) {
        mTimelinePanel.seekAdjacentEditPoint(frameContext(), false);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::NextEditPoint, true)) {
        mTimelinePanel.seekAdjacentEditPoint(frameContext(), true);
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
    if (input::KeyMap::pressed(EditorKeybind::IncreaseUiScale)) {
        setUiScaleTier(steppedUiScaleTier(mUiScaleTier, 1, ImGui::GetIO().DisplaySize.y));
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::DecreaseUiScale)) {
        setUiScaleTier(steppedUiScaleTier(mUiScaleTier, -1, ImGui::GetIO().DisplaySize.y));
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::ResetUiScale)) {
        setUiScaleTier(UiScaleTier::Auto);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::TogglePlayback)) {
        submitAction({playback::state::EditorActionType::TogglePause});
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::MarkExportIn)) {
        markExportPoint(true);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::MarkExportOut)) {
        markExportPoint(false);
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::JumpExportIn)) {
        seekTo(
            state::editing::TimelineNavigation::resolveExportPointTick(
                state().totalTicks,
                exportStartTick(),
                exportEndTick(),
                false
            )
        );
        return;
    }
    if (input::KeyMap::pressed(EditorKeybind::JumpExportOut)) {
        seekTo(
            state::editing::TimelineNavigation::resolveExportPointTick(
                state().totalTicks,
                exportStartTick(),
                exportEndTick(),
                true
            )
        );
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
