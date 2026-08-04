#include "SubActorCommands.h"

#include <algorithm>

namespace playback::editor::editing::command {
SetSubActorDetails::SetSubActorDetails(std::string subActorId, model::AgentDetails details)
    : mSubActorId(std::move(subActorId)), mDetails(std::move(details)) {}
void SetSubActorDetails::execute(model::EditorStateExt& state) {
    mBefore = state;
    auto it = std::find_if(state.worldActor.subActors.begin(), state.worldActor.subActors.end(), [&](const auto& actor) { return actor.id == mSubActorId; });
    if (it != state.worldActor.subActors.end()) it->agentDetails = mDetails;
}
void SetSubActorDetails::undo(model::EditorStateExt& state) { if (mBefore) state = *mBefore; }
std::string SetSubActorDetails::label() const { return "Set SubActor Details"; }
}
