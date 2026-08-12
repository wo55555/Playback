#include "playback/editor/editing/CameraBindingOps.h"
#include "playback/editor/editing/commands/CameraCommands.h"
#include "playback/editor/keyframe/CameraTimelineEvaluator.h"
#include "playback/editor/keyframe/CameraTimelineRegistry.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

using playback::editor::editing::CameraBindingOps::createBindingCamera;
using playback::editor::editing::command::MoveKeyframe;
using playback::editor::editing::command::SetCameraKind;
using playback::editor::editing::command::SetCameraTrackState;
using playback::editor::editing::model::CameraEntity;
using playback::editor::editing::model::CameraKeyframe;
using playback::editor::editing::model::CameraKind;
using playback::editor::editing::model::CameraPath;
using playback::editor::editing::model::CameraPathPoint;
using playback::editor::editing::model::CameraPathType;
using playback::editor::editing::model::EditorStateExt;
using playback::editor::keyframe::CameraRenderState;
using playback::editor::keyframe::CameraTimelineEvaluator;
using playback::editor::keyframe::CameraTimelineSource;
using playback::editor::keyframe::clearCameraTimeline;
using playback::editor::keyframe::publishCameraTimeline;
using playback::editor::keyframe::sampleCameraTimeline;
using playback::editor::keyframe::sampleCameraTimelineRange;
using playback::functions::render::ReplaySampleTime;

bool near(float actual, float expected, float epsilon = 0.001f) { return std::abs(actual - expected) <= epsilon; }

void require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

CameraKeyframe key(std::string id, int tick, float x, float yaw, float roll, float fov) {
    CameraKeyframe result;
    result.id       = std::move(id);
    result.tick     = tick;
    result.position = {x, 0.0f, 0.0f};
    result.yaw      = yaw;
    result.roll     = roll;
    result.fov      = fov;
    return result;
}

CameraRenderState sample(CameraTimelineEvaluator const& evaluator, int64_t numerator, int64_t denominator) {
    auto const result = evaluator.sample(ReplaySampleTime{numerator, denominator});
    require(result.has_value(), "camera sample must exist");
    return *result;
}

void testFractionalLinearSample() {
    EditorStateExt project;
    CameraEntity   camera;
    camera.id   = "camera";
    camera.keys = {key("a", 0, 0.0f, 170.0f, 170.0f, 70.0f), key("b", 10, 10.0f, -170.0f, -170.0f, 90.0f)};
    camera.keys.front().outgoingMotion.fovPeakOffset = 10.0f;
    project.cameras.push_back(camera);

    auto const fractional = sample(CameraTimelineEvaluator(project), 5, 2);
    require(near(fractional.x, 2.5f), "fractional timeline interpolates position");
    require(near(fractional.yaw, 175.0f), "yaw uses the shortest angle");
    require(near(fractional.roll, 175.0f), "roll uses the shortest angle");
    require(near(fractional.fov, 82.07107f), "FOV peak offset affects FOV instead of roll");
}

void testHermiteAndDisabledFallback() {
    EditorStateExt project;
    CameraEntity   disabled;
    disabled.id      = "disabled";
    disabled.enabled = false;
    disabled.keys    = {key("d", 0, 100.0f, 0.0f, 0.0f, 90.0f)};

    CameraEntity enabled;
    enabled.id   = "enabled";
    enabled.keys = {key("a", 0, 0.0f, 0.0f, 0.0f, 90.0f), key("b", 10, 10.0f, 0.0f, 0.0f, 90.0f)};
    enabled.keys.front().outgoingMotion.pathType   = CameraPathType::Hermite;
    enabled.keys.front().outgoingMotion.outControl = {10.0f, 0.0f, 0.0f};
    enabled.keys.front().outgoingMotion.inControl  = {-10.0f, 0.0f, 0.0f};
    enabled.keys.back().outgoingMotion.inControl   = {100.0f, 0.0f, 0.0f};
    CameraEntity incomplete;
    incomplete.id   = "incomplete";
    incomplete.kind = CameraKind::Path;

    project.cameras = {disabled, incomplete, enabled};

    CameraTimelineEvaluator evaluator(project, std::string{"disabled"});
    auto const              middle = sample(evaluator, 5, 1);
    require(near(middle.x, 7.5f), "Hermite uses both tangents from the outgoing segment");
    require(
        !evaluator.sampleCameraById("disabled", ReplaySampleTime{5, 1}).has_value(),
        "exact camera sampling does not fall back from a disabled camera"
    );
    auto const exact = evaluator.sampleCameraById("enabled", ReplaySampleTime{5, 1});
    require(exact.has_value() && near(exact->x, 7.5f), "exact camera sampling uses the requested camera");
}

void testTrackStateAndMoveCommands() {
    EditorStateExt project;
    project.totalTicks = 100;
    CameraEntity camera;
    camera.id   = "camera";
    camera.keys = {key("a", 10, 0.0f, 0.0f, 0.0f, 90.0f), key("b", 20, 1.0f, 0.0f, 0.0f, 90.0f)};
    project.cameras.push_back(camera);

    SetCameraTrackState disable("camera", SetCameraTrackState::Property::Enabled, false);
    disable.execute(project);
    require(disable.didChange() && !project.cameras.front().enabled, "track enabled state changes");
    disable.undo(project);
    require(project.cameras.front().enabled, "track enabled state is undoable");

    project.cameras.front().locked = true;
    SetCameraTrackState hidePath("camera", SetCameraTrackState::Property::PathVisible, false);
    hidePath.execute(project);
    require(
        hidePath.didChange() && !project.cameras.front().pathVisible,
        "track visibility remains available while camera content is locked"
    );
    project.cameras.front().locked = false;

    MoveKeyframe duplicate("camera", "a", 20);
    duplicate.execute(project);
    require(!duplicate.didChange() && project.cameras.front().keys.front().tick == 10, "duplicate ticks are rejected");

    MoveKeyframe move("camera", "a", 15);
    move.execute(project);
    require(move.didChange() && project.cameras.front().keys.front().tick == 15, "keyframe move succeeds");
    move.undo(project);
    require(project.cameras.front().keys.front().tick == 10, "keyframe move is undoable");

    SetCameraKind missingSource("camera", CameraKind::Path);
    missingSource.execute(project);
    require(!missingSource.didChange(), "camera kind rejects a source with no backing data");

    CameraPath path;
    path.points.push_back(CameraPathPoint{
        0,
        {2.0f, 3.0f, 4.0f}
    });
    project.cameras.front().path = path;
    SetCameraKind readySource("camera", CameraKind::Path);
    readySource.execute(project);
    require(
        readySource.didChange() && project.cameras.front().kind == CameraKind::Path,
        "camera kind accepts ready source data"
    );
    readySource.undo(project);
    require(project.cameras.front().kind == CameraKind::Keyframe, "camera kind change is undoable");
}

void testBindingCameraInitialization() {
    EditorStateExt project;
    project.totalTicks = 100;
    project.worldActor.subActors.push_back({
        .id       = "actor",
        .name     = "Actor",
        .position = {12.0f, 64.0f, -8.0f},
        .rotation = {30.0f, -10.0f},
    });

    auto const cameraId = createBindingCamera(project, "actor", {});
    require(!cameraId.empty() && project.cameras.size() == 1, "binding camera is created");
    auto const& camera = project.cameras.front();
    require(camera.kind == CameraKind::Preset && camera.preset.has_value(), "binding camera has a preset source");
    require(playback::editor::editing::model::isCameraRenderable(camera), "binding camera is renderable");
    require(
        project.worldActor.subActors.front().boundCameraIds == std::vector<std::string>{cameraId},
        "binding is recorded on the actor"
    );

    auto const initial = sample(CameraTimelineEvaluator(project, cameraId), 0, 1);
    require(
        near(initial.x, 12.0f) && near(initial.y, 64.0f) && near(initial.z, -8.0f),
        "binding camera starts at the actor's captured position"
    );
    require(near(initial.yaw, 30.0f) && near(initial.pitch, -10.0f), "binding camera captures actor rotation");
}

void testRegistryExactCameraSampling() {
    EditorStateExt project;
    project.totalTicks = 10;

    CameraEntity first;
    first.id   = "first";
    first.keys = {key("first.a", 0, 100.0f, 0.0f, 0.0f, 90.0f)};

    CameraEntity selected;
    selected.id   = "selected";
    selected.keys = {key("selected.a", 0, 0.0f, 0.0f, 0.0f, 90.0f), key("selected.b", 10, 10.0f, 0.0f, 0.0f, 90.0f)};

    CameraEntity disabled = selected;
    disabled.id           = "disabled";
    disabled.enabled      = false;
    project.cameras       = {first, selected, disabled};

    publishCameraTimeline(CameraTimelineSource::Preview, std::make_shared<CameraTimelineEvaluator>(project));
    auto const exact = sampleCameraTimeline(CameraTimelineSource::Preview, ReplaySampleTime{5, 1}, "selected");
    require(exact.has_value() && near(exact->state.x, 5.0f), "registry samples the requested camera");

    auto const path = sampleCameraTimelineRange(CameraTimelineSource::Preview, 0, 10, 3, "selected");
    require(
        path.size() == 3 && near(path.front().x, 0.0f) && near(path[1].x, 5.0f) && near(path.back().x, 10.0f),
        "registry range sampling stays on the requested camera"
    );
    require(
        sampleCameraTimelineRange(CameraTimelineSource::Preview, 0, 10, 3, "disabled").empty(),
        "registry range sampling does not fall back from an invalid camera"
    );
    clearCameraTimeline(CameraTimelineSource::Preview);
}

} // namespace

int main() {
    testFractionalLinearSample();
    testHermiteAndDisabledFallback();
    testTrackStateAndMoveCommands();
    testBindingCameraInitialization();
    testRegistryExactCameraSampling();
    std::cout << "camera timeline tests passed\n";
    return EXIT_SUCCESS;
}
