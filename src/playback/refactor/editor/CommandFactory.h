#pragma once

#include "models/EditorStateExt.h"
#include "models/Track.h"

#include <memory>
#include <string>

namespace playback::refactor::editor {

// Forward declarations
class IEditCommand;

// ===== CommandFactory =====
// Creates IEditCommand instances for common editor operations.
// Used by EditorBridge and TimelinePanel to execute commands
// via the CommandStack.

class CommandFactory {
public:
    // ── Clip commands ──
    static std::unique_ptr<IEditCommand> createSplitClip(
        const std::string& trackId, const std::string& clipId, int atTick);

    static std::unique_ptr<IEditCommand> createRemoveClip(
        const std::string& trackId, const std::string& clipId);

    static std::unique_ptr<IEditCommand> createTrimClip(
        const std::string& trackId, const std::string& clipId,
        int newInTick, int newOutTick);

    static std::unique_ptr<IEditCommand> createMoveClip(
        const std::string& trackId, const std::string& clipId, int newTrackTick);

    // ── Transition commands ──
    static std::unique_ptr<IEditCommand> createAddTransition(
        const std::string& fromClipId, const std::string& toClipId,
        TransitionKind kind, int durationTicks);

    // ── Track commands ──
    static std::unique_ptr<IEditCommand> createAddTrack(
        TrackKind kind, const std::string& name);

    static std::unique_ptr<IEditCommand> createRemoveTrack(
        const std::string& trackId);
};

} // namespace playback::refactor::editor