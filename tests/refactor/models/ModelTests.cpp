#include "playback/refactor/editor/CommandStack.h"
#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/video-editing/CameraBindingOps.h"
#include "playback/refactor/video-editing/SequenceOps.h"
#include "playback/refactor/video-editing/WorldActorOps.h"
#include "playback/refactor/video-editing/commands/CameraCommands.h"
#include "playback/refactor/video-editing/commands/SequenceCommands.h"

#include <cstdlib>
#include <iostream>

namespace {
using namespace playback::refactor;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

editor::EditorStateExt makeState() {
    editor::EditorStateExt state;
    state.totalTicks = 100;
    state.sequence.push_back({"sequence", 0, 100});
    state.worldActor.segments.push_back({"world", 0, 100, 40});
    state.worldActor.totalTicks = 100;
    state.worldActor.subActors.push_back({"actor", "Actor"});
    return state;
}

void testSequenceOps() {
    auto state = makeState();
    auto rightId = video_editing::SequenceOps::splitAt(state.sequence, 40);
    require(!rightId.empty(), "sequence split must create a segment");
    require(video_editing::SequenceOps::validateCoverage(state.sequence, 100), "sequence split must preserve coverage");
    require(video_editing::SequenceOps::findSegmentAt(state.sequence, 40)->id == rightId, "sequence lookup must use half-open boundaries");
    require(video_editing::SequenceOps::deleteSegment(state.sequence, 1, 100), "sequence delete must remove a non-final segment");
    require(video_editing::SequenceOps::validateCoverage(state.sequence, 100), "sequence delete must preserve coverage");
}

void testWorldActorOps() {
    auto state = makeState();
    auto rightId = video_editing::WorldActorOps::splitAt(state.worldActor, 40);
    require(!rightId.empty(), "world actor split must create a segment");
    require(video_editing::WorldActorOps::mapTimelineToSourceTick(state.worldActor, 50) == 90, "world actor split must preserve source mapping");
    require(video_editing::WorldActorOps::setSpeed(state.worldActor, rightId, 2.0f), "world actor speed must accept positive values");
    require(video_editing::WorldActorOps::mapTimelineToSourceTick(state.worldActor, 50) == 100, "world actor mapping must apply speed");
}

void testCameraAndUndo() {
    auto state = makeState();
    editor::CommandStack stack;
    stack.push(std::make_unique<video_editing::AddFreeCamera>("Main"), state);
    require(state.cameras.size() == 1 && stack.canUndo(), "add camera command must modify v3 state");
    const auto cameraId = state.cameras.front().id;
    stack.push(std::make_unique<video_editing::BindSequenceToCamera>("sequence", cameraId), state);
    require(state.sequence.front().cameraId == cameraId, "bind command must set camera id");
    require(stack.undo(state), "bind command must undo");
    require(state.sequence.front().cameraId.empty(), "bind undo must restore the prior sequence");
    stack.push(std::make_unique<video_editing::CreateBindingCamera>("actor", "Actor Follow"), state);
    require(state.cameras.size() == 2 && state.worldActor.subActors.front().boundCameraIds.size() == 1, "binding camera must update both associations");
    require(stack.undo(state), "binding camera must undo");
    require(state.cameras.size() == 1 && state.worldActor.subActors.front().boundCameraIds.empty(), "binding camera undo must restore both associations");
}
}

int main() {
    testSequenceOps();
    testWorldActorOps();
    testCameraAndUndo();
    return 0;
}
