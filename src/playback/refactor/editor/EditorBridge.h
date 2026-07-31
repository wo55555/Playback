#pragma once

#include "models/EditorStateExt.h"
#include "models/SelectionModel.h"
#include "CommandStack.h"
#include "EventBus.h"

#include <string>
#include <vector>

// Forward declarations from old system
namespace playback::editor {
class EditorContext;
struct EditorAction;
} // namespace playback::editor

namespace playback::functions {
class ReplaySession;
} // namespace playback::functions

namespace playback::refactor::editor {

// Forward declarations
class CommandFactory;

// ===== EditorBridge =====
// Bridges the new refactored editor UI with the old business logic system.
// Responsibilities:
//   1. Sync state from old EditorContext → new EditorStateExt
//   2. Submit EditorAction from new UI → old EditorContext (→ EditorController → ReplaySession)
//   3. Execute edit commands (split, trim, delete) via CommandStack
//   4. Dispatch events via EventBus
//
// Usage:
//   EditorBridge::getInstance().initialize(&gContext);
//   EditorBridge::getInstance().syncState(); // each frame before UI draw
//   EditorBridge::getInstance().playPause(); // on UI action
//   EditorBridge::getInstance().commitState(); // each frame after UI edits

class EditorBridge {
public:
    static EditorBridge& getInstance();

    // ── Lifecycle ──
    // Attach to the old editor context (called once from ReplayUI)
    void initialize(playback::editor::EditorContext* context);
    void shutdown();

    [[nodiscard]] bool isInitialized() const { return mContext != nullptr; }

    // ── Frame sync ──
    // Call at the start of each frame: old state → new state
    void syncState(EditorStateExt& outState);
    // Call at the end of each frame: flush pending actions
    void commitState();

    // ── Playback control (→ EditorAction → EditorContext → EditorController → ReplaySession) ──
    void playPause();
    void seek(int tick);
    void skipToStart();
    void skipToEnd();
    void decreaseSpeed();
    void increaseSpeed();
    void stopReplay();

    // ── Edit commands (→ CommandStack → EditorStateExt) ──
    void splitClip(EditorStateExt& state, const std::string& trackId,
                   const std::string& clipId, int atTick);
    void deleteClip(EditorStateExt& state, const std::string& trackId,
                    const std::string& clipId);
    void trimClip(EditorStateExt& state, const std::string& trackId,
                  const std::string& clipId, int newInTick, int newOutTick);
    void moveClip(EditorStateExt& state, const std::string& trackId,
                  const std::string& clipId, int newTrackTick);
    void addTransition(EditorStateExt& state, const std::string& fromClipId,
                       const std::string& toClipId, int kind, int durationTicks);

    // ── Keyframe operations ──
    void addKeyframe(EditorStateExt& state, const std::string& trackId, int tick);
    void moveKeyframe(EditorStateExt& state, const std::string& trackId,
                      const std::string& kfId, int newTick);
    void deleteKeyframe(EditorStateExt& state, const std::string& trackId,
                        const std::string& kfId);

    // ── Marker operations ──
    void addMarker(EditorStateExt& state, const std::string& label, int tick);
    void deleteMarker(EditorStateExt& state, const std::string& markerId);

    // ── Track operations ──
    void addVideoTrack(EditorStateExt& state, const std::string& name);
    void deleteVideoTrack(EditorStateExt& state, const std::string& trackId);

    // ── Initialization ──
    // Populate EditorStateExt with default tracks if empty (called after syncState)
    void ensureInitialData(EditorStateExt& state);

    // ── Undo/Redo ──
    void undo(EditorStateExt& state);
    void redo(EditorStateExt& state);

    // ── Accessors ──
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] CommandStack& commandStack() { return mCommandStack; }

private:
    EditorBridge() = default;

    // Submit an EditorAction to the old context
    void submitAction(playback::editor::EditorAction action);

    playback::editor::EditorContext* mContext{nullptr};
    CommandStack                     mCommandStack;
    std::vector<playback::editor::EditorAction> mPendingActions;
};

} // namespace playback::refactor::editor