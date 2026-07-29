#include "Editor.h"

#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/ErrorDialog.h"
#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"

namespace playback::refactor::editor {

Editor& Editor::getInstance() {
    static Editor instance;
    return instance;
}

void Editor::initialize() {
    KeyMap::initialize();
    mTheme.apply();
    mOpen = false;
}

void Editor::shutdown() {
    mOpen = false;
    mSelection.clear();
    mState = {};
    EditorBridge::getInstance().shutdown();
}

void Editor::toggle() {
    mOpen = !mOpen;
    if (mOpen) {
        mTheme.apply();
    }
}

void Editor::draw() {
    if (!mOpen) return;

    // Lazy font loading (ImGui context must be available)
    static bool sFontsLoaded = false;
    if (!sFontsLoaded) {
        mIconSystem.loadFonts();
        sFontsLoaded = true;
    }

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