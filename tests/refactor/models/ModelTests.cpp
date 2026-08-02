#include "playback/refactor/editor/CommandStack.h"
#include "playback/refactor/editor/CommandFactory.h"
#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/video-editing/CameraBindingOps.h"
#include "playback/refactor/video-editing/SequenceOps.h"
#include "playback/refactor/video-editing/WorldActorOps.h"
#include "playback/refactor/video-editing/commands/CameraCommands.h"
#include "playback/refactor/video-editing/commands/SequenceCommands.h"
#include "playback/refactor/video-editing/commands/SubActorCommands.h"
#include "playback/refactor/video-editing/commands/WorldActorCommands.h"

#include <cstdlib>
#include <iostream>
#include <memory>

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

void testCommandGroups() {
    auto state = makeState();
    editor::CommandStack stack;
    stack.push(std::make_unique<video_editing::SplitSequenceAtPlayhead>(40), state);
    require(state.sequence.size() == 2, "split sequence command must split");
    require(stack.undo(state) && state.sequence.size() == 1, "split sequence undo must restore state");
    require(stack.redo(state) && state.sequence.size() == 2, "split sequence redo must reapply state");
    stack.push(std::make_unique<video_editing::TrimSequenceSegment>("sequence", 0, 30), state);
    require(state.sequence.front().endTick == 30 && state.sequence.back().startTick == 30, "trim sequence must move the shared boundary");
    stack.push(std::make_unique<video_editing::DeleteSequenceSegment>("sequence.split.40"), state);
    require(state.sequence.size() == 1 && video_editing::SequenceOps::validateCoverage(state.sequence, 100), "delete sequence command must preserve coverage");

    state = makeState();
    stack.clear();
    stack.push(std::make_unique<video_editing::SplitWorldActorAtPlayhead>(50), state);
    require(state.worldActor.segments.size() == 2, "split world actor command must split");
    auto worldRight = state.worldActor.segments.back().id;
    stack.push(std::make_unique<video_editing::TrimWorldActorSegment>("world", 0, 40), state);
    require(state.worldActor.segments.front().endTick == 40, "trim world actor command must move the shared boundary");
    stack.push(std::make_unique<video_editing::SetWorldActorSegmentSpeed>(worldRight, 0.5f), state);
    require(state.worldActor.segments.back().speed == 0.5f, "world actor speed command must set speed");
    stack.push(std::make_unique<video_editing::RippleDeleteWorldActorSeg>(worldRight), state);
    require(state.worldActor.segments.size() == 1 && video_editing::WorldActorOps::validateCoverage(state.worldActor, 100), "world actor ripple delete must preserve coverage");

    state = makeState();
    stack.clear();
    stack.push(std::make_unique<video_editing::AddFreeCamera>("Main"), state);
    auto cameraId = state.cameras.front().id;
    stack.push(std::make_unique<video_editing::AddKeyframe>(cameraId, -1), state);
    require(state.cameras.front().keys.size() == 1 && state.cameras.front().keys.front().tick == 0, "keyframe tick must clamp before insertion");
    stack.push(std::make_unique<video_editing::AddKeyframe>(cameraId, 0), state);
    require(state.cameras.front().keys.size() == 1, "normalized duplicate keyframe must be rejected");
    stack.push(std::make_unique<video_editing::AddKeyframe>(cameraId, 50), state);
    auto keyframeId = state.cameras.front().keys.back().id;
    stack.push(std::make_unique<video_editing::MoveKeyframe>(cameraId, keyframeId, 0), state);
    require(state.cameras.front().keys.back().tick == 50, "keyframe move collision must be rejected");
    stack.push(std::make_unique<video_editing::SetKeyframeEasing>(cameraId, keyframeId, editor::EasingType::EaseIn), state);
    require(state.cameras.front().keys.back().easingType == editor::EasingType::EaseIn, "keyframe easing command must update easing");
    stack.push(std::make_unique<video_editing::SetCameraKind>(cameraId, editor::CameraKind::Rig), state);
    require(state.cameras.front().kind == editor::CameraKind::Rig, "camera kind command must update kind");
    stack.push(std::make_unique<video_editing::DeleteKeyframe>(cameraId, keyframeId), state);
    require(state.cameras.front().keys.size() == 1, "delete keyframe command must delete");
    stack.push(std::make_unique<video_editing::CreateBindingCamera>("actor", "Follow"), state);
    auto bindingId = state.cameras.back().id;
    stack.push(std::make_unique<video_editing::UnbindCamera>(bindingId), state);
    require(state.cameras.back().bindingEntityUuid.empty() && state.worldActor.subActors.front().boundCameraIds.empty(), "unbind command must remove both associations");
    stack.push(std::make_unique<video_editing::DeleteCamera>(cameraId), state);
    require(state.cameras.size() == 1, "delete camera command must remove the camera");
    editor::AgentDetails details{{"state", "active"}};
    stack.push(std::make_unique<video_editing::SetSubActorDetails>("actor", details), state);
    require(state.worldActor.subActors.front().agentDetails == details, "sub actor details command must update details");
}

void testFactoryAndStack() {
    auto state = makeState();
    std::vector<std::unique_ptr<editor::IEditCommand>> commands;
    commands.push_back(editor::CommandFactory::createSplitSequence(50));
    commands.push_back(editor::CommandFactory::createTrimSequence("sequence", 0, 80));
    commands.push_back(editor::CommandFactory::createDeleteSequenceSegment("sequence"));
    commands.push_back(editor::CommandFactory::createBindSequenceToCamera("sequence", "camera_1"));
    commands.push_back(editor::CommandFactory::createSplitWorldActor(50));
    commands.push_back(editor::CommandFactory::createTrimWorldActor("world", 0, 80));
    commands.push_back(editor::CommandFactory::createSetWorldActorSpeed("world", 2.0f));
    commands.push_back(editor::CommandFactory::createRippleDeleteWorldActorSegment("world"));
    commands.push_back(editor::CommandFactory::createAddFreeCamera("Main"));
    commands.push_back(editor::CommandFactory::createDeleteCamera("camera_1"));
    commands.push_back(editor::CommandFactory::createCreateBindingCamera("actor", "Follow"));
    commands.push_back(editor::CommandFactory::createUnbindCamera("camera_1"));
    commands.push_back(editor::CommandFactory::createAddCameraKeyframe("camera_1", 50));
    commands.push_back(editor::CommandFactory::createMoveCameraKeyframe("camera_1", "key", 50));
    commands.push_back(editor::CommandFactory::createDeleteCameraKeyframe("camera_1", "key"));
    commands.push_back(editor::CommandFactory::createSetKeyframeEasing("camera_1", "key", editor::EasingType::EaseOut));
    commands.push_back(editor::CommandFactory::createSetCameraKind("camera_1", editor::CameraKind::Path));
    commands.push_back(editor::CommandFactory::createSetSubActorDetails("actor", {}));
    for (const auto& command : commands) require(command != nullptr, "every v3 factory method must return a command");

    editor::CommandStack stack;
    for (int index = 0; index < 101; ++index) stack.push(editor::CommandFactory::createAddFreeCamera("Camera"), state);
    require(stack.undoLabels().size() == 100, "command stack must retain at most 100 steps");
    require(stack.undo(state), "command stack must undo after reaching its limit");
    stack.push(editor::CommandFactory::createAddFreeCamera("Fresh"), state);
    require(!stack.canRedo(), "new command must clear redo history");
}
}

int main() {
    testSequenceOps();
    testWorldActorOps();
    testCameraAndUndo();
    testCommandGroups();
    testFactoryAndStack();
    return 0;
}
