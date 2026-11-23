#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/state/editing/TimelineNavigation.h"
#include "playback/state/editing/commands/CameraCommands.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace playback::keyframe {

namespace {
std::optional<CameraRenderState> gStubCapturedCamera;
}

std::optional<CameraRenderState> captureClientCamera() noexcept { return gStubCapturedCamera; }

} // namespace playback::keyframe

namespace {

using namespace playback::state::editing;
using namespace playback::state::editing::command;
using namespace playback::state::editing::model;

int gFailureCount = 0;

void check(bool condition, char const* expr, int line) {
    if (condition) return;
    std::cerr << "FAIL line " << line << ": " << expr << '\n';
    ++gFailureCount;
}

#define CHECK(cond) check((cond), #cond, __LINE__)

EditorStateExt makeTestState(int totalTicks = 200) {
    EditorStateExt state;
    state.totalTicks = totalTicks;
    CameraEntity cam;
    cam.id   = "cam_1";
    cam.name = "Camera 1";
    state.cameras.push_back(cam);
    return state;
}

void testTimelineNavigationSmartEdges() {
    using namespace TimelineNavigation;

    // Empty or non-positive replay length always resolves to 0.
    CHECK(resolveSmartEdgeTick(50, 0, 10, 90, false) == 0);
    CHECK(resolveSmartEdgeTick(50, 0, 10, 90, true) == 0);
    CHECK(resolveSmartEdgeTick(10, -5, 1, 2, false) == 0);

    // Home / SkipStart with interior In point [20, 80] in total=200.
    CHECK(resolveSmartEdgeTick(150, 200, 20, 80, false) == 20);
    CHECK(resolveSmartEdgeTick(21, 200, 20, 80, false) == 20);
    CHECK(resolveSmartEdgeTick(20, 200, 20, 80, false) == 0);
    CHECK(resolveSmartEdgeTick(5, 200, 20, 80, false) == 0);
    CHECK(resolveSmartEdgeTick(0, 200, 20, 80, false) == 0);

    // End / SkipEnd with interior Out point [20, 80] in total=200.
    CHECK(resolveSmartEdgeTick(0, 200, 20, 80, true) == 80);
    CHECK(resolveSmartEdgeTick(79, 200, 20, 80, true) == 80);
    CHECK(resolveSmartEdgeTick(80, 200, 20, 80, true) == 200);
    CHECK(resolveSmartEdgeTick(150, 200, 20, 80, true) == 200);
    CHECK(resolveSmartEdgeTick(200, 200, 20, 80, true) == 200);

    // When In==0 or Out==total, smart edge jumps directly to 0 / total.
    CHECK(resolveSmartEdgeTick(50, 200, 0, 80, false) == 0);
    CHECK(resolveSmartEdgeTick(50, 200, 20, 200, true) == 200);

    // Unset In/Out (-1) still allows single-ended smart stop on the valid side.
    CHECK(resolveSmartEdgeTick(100, 200, 30, -1, false) == 30);
    CHECK(resolveSmartEdgeTick(100, 200, -1, 150, true) == 150);

    // Inverted range (in >= out) ignores interior stops.
    CHECK(resolveSmartEdgeTick(100, 200, 80, 80, false) == 0);
    CHECK(resolveSmartEdgeTick(100, 200, 90, 80, true) == 200);
}

void testTimelineNavigationExportPointJump() {
    using namespace TimelineNavigation;

    CHECK(resolveExportPointTick(0, 10, 50, false) == 0);
    CHECK(resolveExportPointTick(0, 10, 50, true) == 0);
    CHECK(resolveExportPointTick(200, -1, -1, false) == 0);
    CHECK(resolveExportPointTick(200, -1, -1, true) == 200);
    CHECK(resolveExportPointTick(200, 25, 175, false) == 25);
    CHECK(resolveExportPointTick(200, 25, 175, true) == 175);
    CHECK(resolveExportPointTick(200, 999, 999, false) == 200);
    CHECK(resolveExportPointTick(200, 999, 999, true) == 200);
}

void testAddKeyframeOverwriteAndUndo() {
    EditorStateExt state = makeTestState(200);

    CameraKeyframe first;
    first.position          = {1.0f, 2.0f, 3.0f};
    first.yaw               = 45.0f;
    first.fov               = 65.0f;
    first.mode              = CameraKeyframeMode::Interpolated;
    first.interpolationType = CameraInterpolationType::CubicBezier;

    AddKeyframe addCmd("cam_1", 40, first);
    addCmd.execute(state);
    CHECK(addCmd.didChange());
    CHECK(addCmd.label() == "playback.refactorEditor.history.addKeyframe");
    CHECK(state.cameras[0].keysByTick.size() == 1);
    CHECK(state.cameras[0].keysByTick.at(40).position.x == 1.0f);

    CameraKeyframe replacement;
    replacement.position = {10.0f, 20.0f, 30.0f};
    replacement.yaw      = 90.0f;
    replacement.fov      = 80.0f;
    replacement.mode     = CameraKeyframeMode::ThirdPerson;
    replacement.subject  = "player-uuid-1";

    AddKeyframe replaceCmd("cam_1", 40, replacement);
    replaceCmd.execute(state);
    CHECK(replaceCmd.didChange());
    CHECK(replaceCmd.label() == "playback.refactorEditor.history.replaceKeyframe");
    CHECK(state.cameras[0].keysByTick.size() == 1);
    CHECK(state.cameras[0].keysByTick.at(40).position.x == 10.0f);
    CHECK(state.cameras[0].keysByTick.at(40).mode == CameraKeyframeMode::ThirdPerson);
    CHECK(state.cameras[0].keysByTick.at(40).subject == "player-uuid-1");

    replaceCmd.undo(state);
    CHECK(state.cameras[0].keysByTick.size() == 1);
    CHECK(state.cameras[0].keysByTick.at(40).position.x == 1.0f);
    CHECK(state.cameras[0].keysByTick.at(40).yaw == 45.0f);
    CHECK(state.cameras[0].keysByTick.at(40).fov == 65.0f);
    CHECK(state.cameras[0].keysByTick.at(40).mode == CameraKeyframeMode::Interpolated);
    CHECK(state.cameras[0].keysByTick.at(40).interpolationType == CameraInterpolationType::CubicBezier);

    // Locked camera rejects add/overwrite.
    state.cameras[0].locked = true;
    AddKeyframe lockedCmd("cam_1", 40, replacement);
    lockedCmd.execute(state);
    CHECK(!lockedCmd.didChange());
    state.cameras[0].locked = false;

    // Uncaptured add on an empty camera uses captureClientCamera() FOV when available.
    EditorStateExt emptyState               = makeTestState(200);
    playback::keyframe::gStubCapturedCamera = playback::keyframe::CameraRenderState{0, 0, 0, 0, 0, 0, 92.5f};
    AddKeyframe capturedFovCmd("cam_1", 10);
    capturedFovCmd.execute(emptyState);
    playback::keyframe::gStubCapturedCamera.reset();
    CHECK(capturedFovCmd.didChange());
    CHECK(emptyState.cameras[0].keysByTick.at(10).fov == 92.5f);

    // Uncaptured add on a non-empty camera defaults new keyframe to Interpolated even if last key was 1P.
    CameraKeyframe fpKey;
    fpKey.mode    = CameraKeyframeMode::FirstPerson;
    fpKey.subject = "uuid-a";
    emptyState.cameras[0].keysByTick.insert_or_assign(20, fpKey);
    AddKeyframe inheritCmd("cam_1", 30);
    inheritCmd.execute(emptyState);
    CHECK(inheritCmd.didChange());
    CHECK(emptyState.cameras[0].keysByTick.at(30).mode == CameraKeyframeMode::Interpolated);
}

void testSetKeyframeModeAutoFillSubject() {
    EditorStateExt state = makeTestState(200);
    CameraKeyframe k10;
    k10.mode = CameraKeyframeMode::Interpolated;
    CameraKeyframe k20;
    k20.mode    = CameraKeyframeMode::FirstPerson;
    k20.subject = "uuid-prior";
    CameraKeyframe k30;
    k30.mode = CameraKeyframeMode::Interpolated;
    CameraKeyframe k40;
    k40.mode    = CameraKeyframeMode::Interpolated;
    k40.subject = "uuid-existing";
    state.cameras[0].keysByTick.emplace(10, k10);
    state.cameras[0].keysByTick.emplace(20, k20);
    state.cameras[0].keysByTick.emplace(30, k30);
    state.cameras[0].keysByTick.emplace(40, k40);

    // No prior person keyframe -> falls back to defaultSubject.
    SetCameraKeyframeMode cmd10("cam_1", 10, CameraKeyframeMode::FirstPerson, "uuid-default");
    cmd10.execute(state);
    CHECK(cmd10.didChange());
    CHECK(state.cameras[0].keysByTick.at(10).mode == CameraKeyframeMode::FirstPerson);
    CHECK(state.cameras[0].keysByTick.at(10).subject == "uuid-default");

    // Has prior person keyframe at tick 20 -> inherits "uuid-prior" instead of defaultSubject.
    SetCameraKeyframeMode cmd30("cam_1", 30, CameraKeyframeMode::ThirdPerson, "uuid-default");
    cmd30.execute(state);
    CHECK(cmd30.didChange());
    CHECK(state.cameras[0].keysByTick.at(30).mode == CameraKeyframeMode::ThirdPerson);
    CHECK(state.cameras[0].keysByTick.at(30).subject == "uuid-prior");

    // Existing non-empty subject is preserved on mode switch.
    SetCameraKeyframeMode cmd40("cam_1", 40, CameraKeyframeMode::FirstPerson, "uuid-default");
    cmd40.execute(state);
    CHECK(cmd40.didChange());
    CHECK(state.cameras[0].keysByTick.at(40).subject == "uuid-existing");

    // Undo restores both mode and empty subject.
    cmd30.undo(state);
    CHECK(state.cameras[0].keysByTick.at(30).mode == CameraKeyframeMode::Interpolated);
    CHECK(state.cameras[0].keysByTick.at(30).subject.empty());
}

void testSetKeyframesModeBatchSortingAndSubject() {
    EditorStateExt state = makeTestState(200);
    CameraKeyframe k10;
    k10.mode = CameraKeyframeMode::Interpolated;
    CameraKeyframe k50;
    k50.mode = CameraKeyframeMode::Interpolated;
    state.cameras[0].keysByTick.emplace(10, k10);
    state.cameras[0].keysByTick.emplace(50, k50);

    CameraEntity lockedCam;
    lockedCam.id     = "cam_locked";
    lockedCam.locked = true;
    lockedCam.keysByTick.emplace(10, k10);
    state.cameras.push_back(lockedCam);

    // Pass unsorted refs (50 before 10) to verify deterministic (cameraId, tick) ordering.
    std::vector<KeyframeRef> refs = {
        {"cam_1",      50, std::nullopt},
        {"cam_locked", 10, std::nullopt},
        {"cam_1",      10, std::nullopt},
    };
    SetKeyframesMode batchCmd(refs, CameraKeyframeMode::ThirdPerson, "uuid-follow");
    batchCmd.execute(state);
    CHECK(batchCmd.didChange());
    CHECK(state.cameras[0].keysByTick.at(10).mode == CameraKeyframeMode::ThirdPerson);
    CHECK(state.cameras[0].keysByTick.at(10).subject == "uuid-follow");
    CHECK(state.cameras[0].keysByTick.at(50).mode == CameraKeyframeMode::ThirdPerson);
    CHECK(state.cameras[0].keysByTick.at(50).subject == "uuid-follow");
    CHECK(state.cameras[1].keysByTick.at(10).mode == CameraKeyframeMode::Interpolated);

    // Setting subject to "" (Recorder) works and skips Interpolated keyframes.
    SetKeyframesSubject clearSubjectCmd(refs, "");
    clearSubjectCmd.execute(state);
    CHECK(clearSubjectCmd.didChange());
    CHECK(state.cameras[0].keysByTick.at(10).subject.empty());
    CHECK(state.cameras[0].keysByTick.at(50).subject.empty());

    clearSubjectCmd.undo(state);
    CHECK(state.cameras[0].keysByTick.at(10).subject == "uuid-follow");
    batchCmd.undo(state);
    CHECK(state.cameras[0].keysByTick.at(10).mode == CameraKeyframeMode::Interpolated);
    CHECK(state.cameras[0].keysByTick.at(50).mode == CameraKeyframeMode::Interpolated);
}

} // namespace

void runTimeOfDayTests();

int main() {
    testTimelineNavigationSmartEdges();
    testTimelineNavigationExportPointJump();
    testAddKeyframeOverwriteAndUndo();
    testSetKeyframeModeAutoFillSubject();
    testSetKeyframesModeBatchSortingAndSubject();
    runTimeOfDayTests();
    if (gFailureCount != 0) {
        std::cerr << gFailureCount << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All editor unit tests passed.\n";
    return EXIT_SUCCESS;
}
