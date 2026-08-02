#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"
#include "playback/editor/editing/models/Track.h"

#include <memory>
#include <string>

namespace playback::editor::editing::command {

// ===== CommandFactory =====
// Creates IEditCommand instances for common editor operations.
// Retained for the future editing backend.

class CommandFactory {
public:
    // ── Clip commands ──
    static std::unique_ptr<model::IEditCommand> createSplitClip(
        const std::string& trackId, const std::string& clipId, int atTick);

    static std::unique_ptr<model::IEditCommand> createRemoveClip(
        const std::string& trackId, const std::string& clipId);

    static std::unique_ptr<model::IEditCommand> createTrimClip(
        const std::string& trackId, const std::string& clipId,
        int newInTick, int newOutTick);

    static std::unique_ptr<model::IEditCommand> createMoveClip(
        const std::string& trackId, const std::string& clipId, int newTrackTick);

    // ── Transition commands ──
    static std::unique_ptr<model::IEditCommand> createAddTransition(
        const std::string& fromClipId, const std::string& toClipId,
        model::TransitionKind kind, int durationTicks);

    // ── Track commands ──
    static std::unique_ptr<model::IEditCommand> createAddTrack(
        model::TrackKind kind, const std::string& name);

    static std::unique_ptr<model::IEditCommand> createRemoveTrack(
        const std::string& trackId);
};

} // namespace playback::editor::editing::command
