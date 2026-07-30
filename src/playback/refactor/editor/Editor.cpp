#include "Editor.h"

#include "playback/Playback.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/ErrorDialog.h"
#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace playback::refactor::editor {

Editor& Editor::getInstance() {
    static Editor instance;
    return instance;
}

void Editor::initialize() {
    KeyMap::initialize();
    // DO NOT call mTheme.apply() here — ImGui context is not yet created
    // during enable(). Theme is applied in draw() each frame.
    mOpen = false;
    loadLayoutPreferences();
}

void Editor::shutdown() {
    saveLayoutPreferences();
    mOpen = false;
    mSelection.clear();
    mState = {};
    EditorBridge::getInstance().shutdown();
}

void Editor::setVideoAspectRatio(float aspectRatio) {
    mVideoAspectRatio = std::clamp(aspectRatio, 0.25f, 4.0f);
    mViewportPanel.setVideoAspectRatio(mVideoAspectRatio);
    saveLayoutPreferences();
}

void Editor::loadLayoutPreferences() {
    std::ifstream input("mods/playback/editor-layout.ini");
    float detailsRatio{};
    float timelineRatio{};
    float aspectRatio{};
    float trackListRatio{};
    float pixelsPerTick{};
    float horizontalScroll{};
    std::string version;
    if (input >> version >> detailsRatio >> timelineRatio >> aspectRatio >> trackListRatio >> pixelsPerTick >> horizontalScroll
        && version == "v3"
        && std::isfinite(detailsRatio) && std::isfinite(timelineRatio) && std::isfinite(aspectRatio)
        && std::isfinite(trackListRatio) && std::isfinite(pixelsPerTick) && std::isfinite(horizontalScroll)) {
        mDetailsWidthRatio = std::clamp(detailsRatio, 0.15f, 0.50f);
        mTimelineHeightRatio = std::clamp(timelineRatio, 0.18f, 0.65f);
        mVideoAspectRatio = std::clamp(aspectRatio, 0.25f, 4.0f);
        mTimelinePanel.setViewPreferences(trackListRatio, pixelsPerTick, horizontalScroll);
    } else {
        input.clear();
        input.seekg(0);
        if (input >> detailsRatio >> timelineRatio >> aspectRatio
            && std::isfinite(detailsRatio) && std::isfinite(timelineRatio) && std::isfinite(aspectRatio)) {
            mDetailsWidthRatio = std::clamp(detailsRatio, 0.15f, 0.50f);
            mTimelineHeightRatio = std::clamp(timelineRatio, 0.18f, 0.65f);
            mVideoAspectRatio = std::clamp(aspectRatio, 0.25f, 4.0f);
        }
    }
    mViewportPanel.setVideoAspectRatio(mVideoAspectRatio);
}

void Editor::saveLayoutPreferences() const {
    std::ofstream output("mods/playback/editor-layout.ini", std::ios::trunc);
    if (output) {
        output << "v3 " << mDetailsWidthRatio << ' ' << mTimelineHeightRatio << ' ' << mVideoAspectRatio << ' '
               << mTimelinePanel.trackListWidthRatio() << ' ' << mTimelinePanel.pixelsPerTick() << ' '
               << mTimelinePanel.horizontalScroll();
    }
}

void Editor::toggle() {
    auto& logger = Playback::getInstance().getSelf().getLogger();
    logger.info("Editor::toggle: mOpen={} -> {}", mOpen, !mOpen);
    FILE* f = nullptr;
    fopen_s(&f, "mods/playback/debug_log.txt", "a");
    if (f) { fprintf(f, "[Editor] toggle: %d -> %d\n", mOpen, !mOpen); fclose(f); }

    mOpen = !mOpen;
    // DO NOT call mTheme.apply() here — ImGui context may not be available yet.
    // Theme is applied each frame in draw().
}

void Editor::open() {
    auto& logger = Playback::getInstance().getSelf().getLogger();
    logger.info("Editor::open: mOpen={}", mOpen);
    FILE* f = nullptr;
    fopen_s(&f, "mods/playback/debug_log.txt", "a");
    if (f) { fprintf(f, "[Editor] open: mOpen=%d\n", mOpen); fclose(f); }

    if (!mOpen) {
        logger.info("Editor::open: calling toggle()...");
        if (FILE* f2 = nullptr; fopen_s(&f2, "mods/playback/debug_log.txt", "a") == 0) {
            fprintf(f2, "[Editor] open: calling toggle()\n");
            fclose(f2);
        }
        toggle();
        logger.info("Editor::open: toggle() done");
        if (FILE* f2 = nullptr; fopen_s(&f2, "mods/playback/debug_log.txt", "a") == 0) {
            fprintf(f2, "[Editor] open: toggle() done\n");
            fclose(f2);
        }
    }
}

void Editor::draw() {
    if (!mOpen) return;

    // Fonts are assembled by ImGuiRenderer before the DX12 backend creates its font texture.

    // Apply theme each frame
    mTheme.apply();

    // ── Sync state from old business layer → new state ──
    EditorBridge::getInstance().syncState(mState);

    // ── Ensure EditorStateExt has initial data (tracks, etc.) ──
    EditorBridge::getInstance().ensureInitialData(mState);

    // ── Delegate to current mode ──
    if (mModeManager.current() == EditorMode::Edit) {
        mEditMode.draw();
    } else {
        mRenderMode.draw();
    }

    // ── Flush pending actions back to old business layer ──
    EditorBridge::getInstance().commitState();

    // Draw error dialog overlay (if active)
    ErrorDialog::getInstance().draw();

    // ── Keyboard shortcuts (processed after all UI panels are drawn) ──
    handleKeyboardShortcuts();
}

void Editor::handleKeyboardShortcuts() {
    auto& bridge = EditorBridge::getInstance();
    ImGuiIO& io = ImGui::GetIO();

    // ── Playback control ──
    if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        bridge.playPause();
    }

    // ── Seek ──
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        bridge.skipToStart();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        bridge.skipToEnd();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        int step = io.KeyShift ? 20 : 1;
        bridge.seek(std::max(0, mState.currentTick - step));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        int step = io.KeyShift ? 20 : 1;
        bridge.seek(std::min(mState.totalTicks, mState.currentTick + step));
    }

    // ── Speed ──
    // Note: -/= are handled as separate checks since IsKeyPressed consumes the event
    if (ImGui::IsKeyPressed(ImGuiKey_Minus)) {
        bridge.decreaseSpeed();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal)) {
        bridge.increaseSpeed();
    }

    // ── Undo / Redo ──
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        bridge.undo(mState);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        bridge.redo(mState);
    }

    // ── Delete ──
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        // Delete selected clip (handled via TimelinePanel::handleRippleDelete)
        // This is a placeholder — the actual delete action is triggered from the
        // TimelinePanel which has access to the selected clip ID.
    }

    // ── Escape ──
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        mSelection.clear();
    }

    // ── F1: Toggle hint bar ──
    // (handled by InputHook::onWindowsMessage for toggleUI)
}

} // namespace playback::refactor::editor
