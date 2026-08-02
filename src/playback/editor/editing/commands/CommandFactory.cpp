#include "CommandFactory.h"

#include "playback/editor/editing/commands/EditingCommands.h"

namespace playback::editor::editing::command {

// ===== Clip commands =====

std::unique_ptr<model::IEditCommand> CommandFactory::createSplitClip(
    const std::string& trackId, const std::string& clipId, int atTick)
{
    return std::make_unique<SplitClipCommand>(trackId, clipId, atTick);
}

std::unique_ptr<model::IEditCommand> CommandFactory::createRemoveClip(
    const std::string& trackId, const std::string& clipId)
{
    return std::make_unique<RemoveClipCommand>(trackId, clipId);
}

std::unique_ptr<model::IEditCommand> CommandFactory::createTrimClip(
    const std::string& trackId, const std::string& clipId,
    int newInTick, int newOutTick)
{
    return std::make_unique<TrimClipCommand>(trackId, clipId, newInTick, newOutTick);
}

std::unique_ptr<model::IEditCommand> CommandFactory::createMoveClip(
    const std::string& trackId, const std::string& clipId, int newTrackTick)
{
    return std::make_unique<MoveClipCommand>(trackId, clipId, newTrackTick);
}

// ===== Transition commands =====

std::unique_ptr<model::IEditCommand> CommandFactory::createAddTransition(
    const std::string& fromClipId, const std::string& toClipId,
    model::TransitionKind kind, int durationTicks)
{
    return std::make_unique<AddTransitionCommand>(fromClipId, toClipId, kind, durationTicks);
}

// ===== Track commands =====

std::unique_ptr<model::IEditCommand> CommandFactory::createAddTrack(
    model::TrackKind kind, const std::string& name)
{
    // Placeholder — AddTrackCommand will be defined in future iterations
    (void)kind; (void)name;
    return nullptr;
}

std::unique_ptr<model::IEditCommand> CommandFactory::createRemoveTrack(
    const std::string& trackId)
{
    // Placeholder — RemoveTrackCommand will be defined in future iterations
    (void)trackId;
    return nullptr;
}

} // namespace playback::editor::editing::command
