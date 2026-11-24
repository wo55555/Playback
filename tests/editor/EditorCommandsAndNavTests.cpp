#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/keyframe/KeyframeTrack.h"
#include "playback/state/editing/ClipOps.h"
#include "playback/state/editing/TimelineNavigation.h"
#include "playback/state/editing/commands/CameraCommands.h"
#include "playback/state/editing/commands/ClipCommands.h"
#include "playback/state/editing/models/SelectionModel.h"

#include <cmath>
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

Clip makeClip(std::string id, int inTick, int outTick, bool exportEnabled = true, bool locked = false) {
    Clip clip;
    clip.id            = std::move(id);
    clip.name          = clip.id;
    clip.inTick        = inTick;
    clip.outTick       = outTick;
    clip.exportEnabled = exportEnabled;
    clip.locked        = locked;
    return clip;
}

void testCollectCameraClipCuts() {
    CameraEntity cam;
    cam.id = "cam_1";

    // Non-positive totalTicks -> empty.
    CHECK(ClipOps::collectCameraClipCuts(cam, 0).empty());

    // 0 clips -> full gap [0, totalTicks].
    {
        auto const cuts = ClipOps::collectCameraClipCuts(cam, 200);
        CHECK(cuts.size() == 1);
        CHECK((cuts[0] == SelectedClipCut{"cam_1", 0, 200}));
        CHECK(cuts[0].isGap());
        CHECK(!cuts[0].isSeam());
    }

    // Full range clip -> zero cuts.
    cam.clips = {makeClip("clip_1", 0, 200)};
    CHECK(ClipOps::collectCameraClipCuts(cam, 200).empty());

    // 1 split -> 1 seam.
    cam.clips = {makeClip("clip_1", 0, 80), makeClip("clip_2", 80, 200)};
    {
        auto const cuts = ClipOps::collectCameraClipCuts(cam, 200);
        CHECK(cuts.size() == 1);
        CHECK((cuts[0] == SelectedClipCut{"cam_1", 80, 80}));
        CHECK(cuts[0].isSeam());
        CHECK(!cuts[0].isGap());
    }

    // 2 splits -> 2 seams.
    cam.clips = {makeClip("clip_1", 0, 60), makeClip("clip_2", 60, 140), makeClip("clip_3", 140, 200)};
    {
        auto const cuts = ClipOps::collectCameraClipCuts(cam, 200);
        CHECK(cuts.size() == 2);
        CHECK((cuts[0] == SelectedClipCut{"cam_1", 60, 60}));
        CHECK((cuts[1] == SelectedClipCut{"cam_1", 140, 140}));
    }

    // Deleted middle clip -> 1 gap.
    cam.clips = {makeClip("clip_1", 0, 60), makeClip("clip_3", 140, 200)};
    {
        auto const cuts = ClipOps::collectCameraClipCuts(cam, 200);
        CHECK(cuts.size() == 1);
        CHECK((cuts[0] == SelectedClipCut{"cam_1", 60, 140}));
        CHECK(cuts[0].isGap());
    }

    // Trimmed start/end -> 2 gaps.
    cam.clips = {makeClip("clip_1", 25, 175)};
    {
        auto const cuts = ClipOps::collectCameraClipCuts(cam, 200);
        CHECK(cuts.size() == 2);
        CHECK((cuts[0] == SelectedClipCut{"cam_1", 0, 25}));
        CHECK((cuts[1] == SelectedClipCut{"cam_1", 175, 200}));
    }
}

void testSelectionModelClipCutsAndBoxSelection() {
    EditorStateExt state = makeTestState(200);
    state.activeSceneId  = "scene_1";
    state.cameras[0].keysByTick.emplace(20, CameraKeyframe{});
    state.cameras[0].keysByTick.emplace(120, CameraKeyframe{});
    state.cameras[0].clips = {
        makeClip("clip_1", 0, 60),
        makeClip("clip_2", 60, 130),
        makeClip("clip_3", 160, 200),
    };

    SelectionModel sel;

    // Single SelectedClipCut selection + selectedIds.
    sel.select(SelectedClipCut{"cam_1", 60, 60});
    CHECK(sel.hasSelection());
    CHECK(sel.clipCutCount() == 1);
    CHECK(sel.isClipCutSelected("cam_1", 60, 60));
    CHECK((sel.selectedIds() == std::vector<std::string>{"cam_1", "60", "60"}));

    // setClipCuts deduplicates and clears keyframes.
    sel.setClipCuts({
        {"cam_1", 60,  60 },
        {"cam_1", 130, 160},
        {"cam_1", 60,  60 }
    });
    CHECK(sel.keyframeCount() == 0);
    CHECK(sel.clipCutCount() == 2);
    CHECK(sel.getAs<SelectedClipCut>() != nullptr);
    CHECK((*sel.getAs<SelectedClipCut>() == SelectedClipCut{"cam_1", 130, 160}));

    // setBoxSelection stores both keyframes and cuts, deduplicating both, preferring last keyframe as primary.
    sel.setBoxSelection(
        {
            {"cam_1", 20 },
            {"cam_1", 120},
            {"cam_1", 20 }
    },
        {{"cam_1", 60, 60}, {"cam_1", 130, 160}, {"cam_1", 130, 160}}
    );
    CHECK(sel.keyframeCount() == 2);
    CHECK(sel.clipCutCount() == 2);
    CHECK(sel.isKeyframeSelected("cam_1", 20));
    CHECK(sel.isKeyframeSelected("cam_1", 120));
    CHECK(sel.isClipCutSelected("cam_1", 60, 60));
    CHECK(sel.isClipCutSelected("cam_1", 130, 160));
    CHECK(sel.getAs<SelectedKeyframe>() != nullptr);
    CHECK(sel.getAs<SelectedKeyframe>()->tick == 120);

    // Toggling off all keyframes falls back primary selection to mClipCuts.back().
    sel.toggleKeyframe({"cam_1", 20});
    sel.toggleKeyframe({"cam_1", 120});
    CHECK(sel.keyframeCount() == 0);
    CHECK(sel.clipCutCount() == 2);
    CHECK(sel.hasSelection());
    CHECK(sel.getAs<SelectedClipCut>() != nullptr);
    CHECK((*sel.getAs<SelectedClipCut>() == SelectedClipCut{"cam_1", 130, 160}));

    // Mixed pruneInvalid: remove one keyframe and invalidate one seam while keeping one keyframe and one gap.
    sel.setBoxSelection(
        {
            {"cam_1", 20 },
            {"cam_1", 120}
    },
        {{"cam_1", 60, 60}, {"cam_1", 130, 160}}
    );
    state.cameras[0].keysByTick.erase(120);
    state.cameras[0].clips = {
        makeClip("clip_1", 0, 130),
        makeClip("clip_3", 160, 200),
    };
    sel.pruneInvalid(state);
    CHECK(sel.keyframeCount() == 1);
    CHECK(sel.isKeyframeSelected("cam_1", 20));
    CHECK(sel.clipCutCount() == 1);
    CHECK(sel.isClipCutSelected("cam_1", 130, 160));
    CHECK(!sel.isClipCutSelected("cam_1", 60, 60));
    CHECK(sel.getAs<SelectedKeyframe>() != nullptr);
    CHECK(sel.getAs<SelectedKeyframe>()->tick == 20);

    // Invalidating all remaining selections clears selection.
    state.cameras[0].keysByTick.erase(20);
    state.cameras[0].clips = {makeClip("clip_1", 0, 200)};
    sel.pruneInvalid(state);
    CHECK(!sel.hasSelection());
    CHECK(sel.keyframeCount() == 0);
    CHECK(sel.clipCutCount() == 0);

    // Empty box selection clears.
    sel.setClipCuts({
        {"cam_1", 0, 200}
    });
    sel.setBoxSelection({}, {});
    CHECK(!sel.hasSelection());
}

void testHealClipCuts() {
    // 1) Single seam restores full replay and ORs exportEnabled.
    {
        EditorStateExt state   = makeTestState(200);
        state.cameras[0].clips = {makeClip("c1", 0, 80, false), makeClip("c2", 80, 200, true)};
        HealClipCuts cmd({
            {"cam_1", 80, 80}
        });
        cmd.execute(state);
        CHECK(cmd.didChange());
        CHECK(cmd.label() == "playback.refactorEditor.history.restoreClips");
        CHECK(state.cameras[0].clips.size() == 1);
        CHECK(state.cameras[0].clips[0].inTick == 0);
        CHECK(state.cameras[0].clips[0].outTick == 200);
        CHECK(state.cameras[0].clips[0].exportEnabled);
        CHECK(ClipOps::collectCameraClipCuts(state.cameras[0], 200).empty());

        cmd.undo(state);
        CHECK(state.cameras[0].clips.size() == 2);
        CHECK(!state.cameras[0].clips[0].exportEnabled);
    }

    // 2) Partial 1-of-2 seams and both seams.
    {
        EditorStateExt state   = makeTestState(200);
        state.cameras[0].clips = {
            makeClip("c1", 0, 60),
            makeClip("c2", 60, 140),
            makeClip("c3", 140, 200),
        };
        HealClipCuts partialCmd({
            {"cam_1", 60, 60}
        });
        partialCmd.execute(state);
        CHECK(partialCmd.didChange());
        CHECK(state.cameras[0].clips.size() == 2);
        CHECK(state.cameras[0].clips[0].inTick == 0);
        CHECK(state.cameras[0].clips[0].outTick == 140);
        CHECK(state.cameras[0].clips[1].inTick == 140);
        CHECK(state.cameras[0].clips[1].outTick == 200);

        partialCmd.undo(state);
        CHECK(state.cameras[0].clips.size() == 3);

        HealClipCuts bothCmd({
            {"cam_1", 140, 140},
            {"cam_1", 60,  60 }
        });
        bothCmd.execute(state);
        CHECK(bothCmd.didChange());
        CHECK(state.cameras[0].clips.size() == 1);
        CHECK(state.cameras[0].clips[0].inTick == 0);
        CHECK(state.cameras[0].clips[0].outTick == 200);
    }

    // 3) Deleted-clip gap bridges to full replay.
    {
        EditorStateExt state   = makeTestState(200);
        state.cameras[0].clips = {makeClip("c1", 0, 60), makeClip("c3", 140, 200)};
        HealClipCuts cmd({
            {"cam_1", 60, 140}
        });
        cmd.execute(state);
        CHECK(cmd.didChange());
        CHECK(state.cameras[0].clips.size() == 1);
        CHECK(state.cameras[0].clips[0].inTick == 0);
        CHECK(state.cameras[0].clips[0].outTick == 200);
    }

    // 4) Trimmed edges (start + end gaps) restore full replay.
    {
        EditorStateExt state   = makeTestState(200);
        state.cameras[0].clips = {makeClip("c1", 30, 170)};
        HealClipCuts cmd({
            {"cam_1", 170, 200},
            {"cam_1", 0,   30 }
        });
        cmd.execute(state);
        CHECK(cmd.didChange());
        CHECK(state.cameras[0].clips.size() == 1);
        CHECK(state.cameras[0].clips[0].inTick == 0);
        CHECK(state.cameras[0].clips[0].outTick == 200);
    }

    // 5) Empty clips restore full replay.
    {
        EditorStateExt state = makeTestState(200);
        HealClipCuts   cmd({
            {"cam_1", 0, 200}
        });
        cmd.execute(state);
        CHECK(cmd.didChange());
        CHECK(state.cameras[0].clips.size() == 1);
        CHECK(state.cameras[0].clips[0].inTick == 0);
        CHECK(state.cameras[0].clips[0].outTick == 200);
        CHECK(state.cameras[0].clips[0].exportEnabled);
        cmd.undo(state);
        CHECK(state.cameras[0].clips.empty());
    }

    // 6) Combined keyframes + cuts deleted & healed in one command + undo restores both.
    {
        EditorStateExt state = makeTestState(200);
        state.cameras[0].keysByTick.emplace(40, CameraKeyframe{});
        state.cameras[0].keysByTick.emplace(100, CameraKeyframe{});
        state.cameras[0].clips = {makeClip("c1", 0, 80), makeClip("c2", 80, 200)};

        HealClipCuts cmd(
            {
                {"cam_1", 80, 80}
        },
            {{"cam_1", 40, std::nullopt}}
        );
        cmd.execute(state);
        CHECK(cmd.didChange());
        CHECK(cmd.label() == "playback.refactorEditor.history.deleteKeyframesAndRestoreClips");
        CHECK(state.cameras[0].clips.size() == 1);
        CHECK(state.cameras[0].clips[0].outTick == 200);
        CHECK(!state.cameras[0].keysByTick.contains(40));
        CHECK(state.cameras[0].keysByTick.contains(100));

        cmd.undo(state);
        CHECK(state.cameras[0].clips.size() == 2);
        CHECK(state.cameras[0].keysByTick.contains(40));
        CHECK(state.cameras[0].keysByTick.contains(100));
    }

    // 7) Keyframes-only label.
    {
        EditorStateExt state = makeTestState(200);
        state.cameras[0].keysByTick.emplace(50, CameraKeyframe{});
        HealClipCuts cmd({}, {{"cam_1", 50, std::nullopt}});
        cmd.execute(state);
        CHECK(cmd.didChange());
        CHECK(cmd.label() == "playback.refactorEditor.history.deleteKeyframes");
        CHECK(state.cameras[0].keysByTick.empty());
    }

    // 8) Locked camera & locked clip respected.
    {
        EditorStateExt state    = makeTestState(200);
        state.cameras[0].locked = true;
        state.cameras[0].keysByTick.emplace(50, CameraKeyframe{});
        state.cameras[0].clips = {makeClip("c1", 0, 80), makeClip("c2", 80, 200)};

        CameraEntity cam2;
        cam2.id    = "cam_2";
        cam2.clips = {makeClip("c2_1", 0, 80, true, true), makeClip("c2_2", 80, 200)};
        state.cameras.push_back(cam2);

        CameraEntity cam3;
        cam3.id    = "cam_3";
        cam3.clips = {makeClip("c3_1", 0, 60, true, true), makeClip("c3_2", 140, 200)};
        state.cameras.push_back(cam3);

        HealClipCuts cmd(
            {
                {"cam_1", 80, 80 },
                {"cam_2", 80, 80 },
                {"cam_3", 60, 140}
        },
            {{"cam_1", 50, std::nullopt}}
        );
        cmd.execute(state);
        // cam_1 locked -> untouched; cam_2 seam touches locked c2_1 -> untouched; cam_3 gap extends unlocked c3_2 left!
        CHECK(cmd.didChange());
        CHECK(state.cameras[0].clips.size() == 2);
        CHECK(state.cameras[0].keysByTick.contains(50));
        CHECK(state.cameras[1].clips.size() == 2);
        CHECK(state.cameras[2].clips.size() == 2);
        CHECK(state.cameras[2].clips[0].inTick == 0);
        CHECK(state.cameras[2].clips[0].outTick == 60);
        CHECK(state.cameras[2].clips[1].inTick == 60);
        CHECK(state.cameras[2].clips[1].outTick == 200);
    }
}

void testKeyframeTrackOrbitAngleWrapping() {
    using playback::keyframe::KeyframeTrack;
    auto approx = [](float actual, float expected, float eps = 1e-2f) { return std::abs(actual - expected) <= eps; };
    auto makeOrbitKey =
        [](CameraInterpolationType interp, Vec3 offset, float distance, float yaw, float pitch, float roll = 0.0f) {
            CameraKeyframe k;
            k.interpolationType   = interp;
            k.position            = offset;
            k.thirdPersonDistance = distance;
            k.yaw                 = yaw;
            k.pitch               = pitch;
            k.roll                = roll;
            return k;
        };

    // 1) Linear: wrapAngles=false preserves multi-turn yaw (0->360, 0->270) & raw pitch while roll wraps shortest-arc.
    {
        KeyframeTrack::KeyMap keys360{
            {0,  makeOrbitKey(CameraInterpolationType::Linear, {0.0f, 1.0f, -2.0f}, 2.0f, 0.0f,   -20.0f, 170.0f) },
            {20, makeOrbitKey(CameraInterpolationType::Linear, {4.0f, 3.0f, 2.0f},  8.0f, 360.0f, 40.0f,  -170.0f)},
        };
        KeyframeTrack rawTrack360(keys360, false);
        auto const    mid360 = rawTrack360.createChange(10.0L);
        CHECK(mid360.has_value());
        CHECK(approx(mid360->position.x, 2.0f));
        CHECK(approx(mid360->position.y, 2.0f));
        CHECK(approx(mid360->position.z, 0.0f));
        CHECK(approx(mid360->distance(), 5.0f));
        CHECK(approx(mid360->yaw, 180.0f));
        CHECK(approx(mid360->pitch, 10.0f));
        CHECK(approx(std::abs(mid360->roll), 180.0f));

        KeyframeTrack wrappedTrack360(keys360, true);
        auto const    wrappedMid360 = wrappedTrack360.createChange(10.0L);
        CHECK(wrappedMid360.has_value());
        CHECK(approx(wrappedMid360->yaw, 0.0f));

        KeyframeTrack::KeyMap keys270{
            {0,  makeOrbitKey(CameraInterpolationType::Linear, {0.0f, 0.0f, 0.0f}, 2.0f, 0.0f,   0.0f) },
            {20, makeOrbitKey(CameraInterpolationType::Linear, {2.0f, 4.0f, 6.0f}, 6.0f, 270.0f, 30.0f)},
        };
        KeyframeTrack rawTrack270(keys270, false);
        auto const    mid270 = rawTrack270.createChange(10.0L);
        CHECK(mid270.has_value());
        CHECK(approx(mid270->yaw, 135.0f));
        CHECK(approx(mid270->pitch, 15.0f));
        CHECK(approx(mid270->distance(), 4.0f));

        KeyframeTrack wrappedTrack270(keys270, true);
        auto const    wrappedMid270 = wrappedTrack270.createChange(10.0L);
        CHECK(wrappedMid270.has_value());
        CHECK(approx(wrappedMid270->yaw, -45.0f));

        // Free camera shortest-arc across +/-180 seam: 170 -> -170 hits +/-180.
        KeyframeTrack::KeyMap seamKeys{
            {0,  makeOrbitKey(CameraInterpolationType::Linear, {}, 0.0f, 170.0f,  0.0f)},
            {20, makeOrbitKey(CameraInterpolationType::Linear, {}, 0.0f, -170.0f, 0.0f)},
        };
        KeyframeTrack seamWrapped(seamKeys, true);
        auto const    seamMid = seamWrapped.createChange(10.0L);
        CHECK(seamMid.has_value());
        CHECK(approx(std::abs(seamMid->yaw), 180.0f));
    }

    // 2) Smooth (Catmull-Rom): multi-turn yaw & pitch interpolate continuously without wrapping; roll stays
    // shortest-arc.
    {
        KeyframeTrack::KeyMap smoothKeys{
            {0,  makeOrbitKey(CameraInterpolationType::Smooth, {0.0f, 0.0f, 0.0f}, 2.0f, 0.0f,   -30.0f, 170.0f) },
            {20, makeOrbitKey(CameraInterpolationType::Smooth, {2.0f, 1.0f, 0.0f}, 4.0f, 180.0f, -10.0f, -170.0f)},
            {40, makeOrbitKey(CameraInterpolationType::Smooth, {4.0f, 2.0f, 0.0f}, 6.0f, 360.0f, 10.0f,  170.0f) },
            {60, makeOrbitKey(CameraInterpolationType::Smooth, {6.0f, 3.0f, 0.0f}, 8.0f, 540.0f, 30.0f,  -170.0f)},
        };
        KeyframeTrack rawSmooth(smoothKeys, false);
        auto const    mid = rawSmooth.createChange(30.0L);
        CHECK(mid.has_value());
        CHECK(approx(mid->position.x, 3.0f));
        CHECK(approx(mid->position.y, 1.5f));
        CHECK(approx(mid->distance(), 5.0f));
        CHECK(approx(mid->yaw, 270.0f));
        CHECK(approx(mid->pitch, 0.0f));
        CHECK(approx(std::abs(mid->roll), 180.0f));

        KeyframeTrack::KeyMap seamKeys{
            {0,  makeOrbitKey(CameraInterpolationType::Smooth, {}, 0.0f, 170.0f,  0.0f)},
            {20, makeOrbitKey(CameraInterpolationType::Smooth, {}, 0.0f, -170.0f, 0.0f)},
        };
        KeyframeTrack wrappedSmooth(seamKeys, true);
        auto const    seamMid = wrappedSmooth.createChange(10.0L);
        CHECK(seamMid.has_value());
        CHECK(approx(std::abs(seamMid->yaw), 180.0f));
    }

    // 3) Hermite: multi-turn yaw & pitch pass through polynomial without wrapping; roll stays shortest-arc.
    {
        KeyframeTrack::KeyMap hermiteKeys{
            {0,  makeOrbitKey(CameraInterpolationType::Hermite, {0.0f, 0.0f, 0.0f}, 2.0f, 0.0f,   -20.0f, 170.0f) },
            {20, makeOrbitKey(CameraInterpolationType::Hermite, {2.0f, 2.0f, 0.0f}, 5.0f, 270.0f, 0.0f,   -170.0f)},
            {40, makeOrbitKey(CameraInterpolationType::Hermite, {4.0f, 4.0f, 0.0f}, 8.0f, 540.0f, 20.0f,  170.0f) },
        };
        KeyframeTrack rawHermite(hermiteKeys, false);
        auto const    q1 = rawHermite.createChange(10.0L);
        auto const    q3 = rawHermite.createChange(30.0L);
        CHECK(q1.has_value());
        CHECK(q3.has_value());
        CHECK(approx(q1->position.x, 1.0f));
        CHECK(approx(q1->distance(), 3.5f));
        CHECK(approx(q1->yaw, 135.0f));
        CHECK(approx(q1->pitch, -10.0f));
        CHECK(std::abs(q1->roll) > 170.0f);
        CHECK(approx(q3->position.x, 3.0f));
        CHECK(approx(q3->distance(), 6.5f));
        CHECK(approx(q3->yaw, 405.0f));
        CHECK(approx(q3->pitch, 10.0f));

        KeyframeTrack::KeyMap seamKeys{
            {0,  makeOrbitKey(CameraInterpolationType::Hermite, {}, 0.0f, 170.0f,  0.0f)},
            {20, makeOrbitKey(CameraInterpolationType::Hermite, {}, 0.0f, -170.0f, 0.0f)},
        };
        KeyframeTrack wrappedHermite(seamKeys, true);
        auto const    seamMid = wrappedHermite.createChange(10.0L);
        CHECK(seamMid.has_value());
        CHECK(approx(std::abs(seamMid->yaw), 180.0f));
    }

    // 4) CubicBezier: multi-turn yaw, pitch, distance, and offset interpolate smoothly without wrapping.
    {
        KeyframeTrack::KeyMap bezierKeys{
            {0,  makeOrbitKey(CameraInterpolationType::CubicBezier, {0.0f, 1.0f, 2.0f}, 3.0f, 0.0f,   -10.0f, 170.0f) },
            {20, makeOrbitKey(CameraInterpolationType::CubicBezier, {4.0f, 5.0f, 6.0f}, 9.0f, 360.0f, 30.0f,  -170.0f)},
        };
        KeyframeTrack rawBezier(bezierKeys, false);
        auto const    q1  = rawBezier.createChange(5.0L);
        auto const    mid = rawBezier.createChange(10.0L);
        CHECK(q1.has_value());
        CHECK(mid.has_value());
        CHECK(q1->yaw > 0.0f && q1->yaw < 180.0f);
        CHECK(approx(mid->position.x, 2.0f));
        CHECK(approx(mid->position.y, 3.0f));
        CHECK(approx(mid->position.z, 4.0f));
        CHECK(approx(mid->distance(), 6.0f));
        CHECK(approx(mid->yaw, 180.0f));
        CHECK(approx(mid->pitch, 10.0f));
        CHECK(approx(std::abs(mid->roll), 180.0f));
    }
}

void testSetCameraKeyframeThirdPersonSettingsMultiTurnAndClamping() {
    EditorStateExt state = makeTestState(200);
    CameraKeyframe tpKey;
    tpKey.mode              = CameraKeyframeMode::ThirdPerson;
    tpKey.thirdPersonCustom = true;
    state.cameras[0].keysByTick.emplace(40, tpKey);

    CameraKeyframe settings;
    settings.thirdPersonCustom         = true;
    settings.thirdPersonDistance       = -5.0f;
    settings.thirdPersonYaw            = 720.0f;
    settings.thirdPersonPitch          = 120.0f;
    settings.thirdPersonRoll           = 25.0f;
    settings.thirdPersonFov            = 210.0f;
    settings.thirdPersonOffset         = {1.5f, -0.5f, 2.0f};
    settings.thirdPersonFollowRotation = true;
    settings.thirdPersonCollideBlocks  = false;

    SetCameraKeyframeThirdPersonSettings cmd1("cam_1", 40, settings);
    cmd1.execute(state);
    CHECK(cmd1.didChange());
    auto const& k1 = state.cameras[0].keysByTick.at(40);
    CHECK(k1.thirdPersonYaw == 720.0f);
    CHECK(k1.thirdPersonPitch == 89.9f);
    CHECK(k1.thirdPersonDistance == 0.0f);
    CHECK(k1.thirdPersonFov == 179.0f);

    settings.thirdPersonYaw   = -270.0f;
    settings.thirdPersonPitch = -120.0f;
    settings.thirdPersonFov   = 0.1f;
    SetCameraKeyframeThirdPersonSettings cmd2("cam_1", 40, settings);
    cmd2.execute(state);
    CHECK(cmd2.didChange());
    auto const& k2 = state.cameras[0].keysByTick.at(40);
    CHECK(k2.thirdPersonYaw == -270.0f);
    CHECK(k2.thirdPersonPitch == -89.9f);
    CHECK(k2.thirdPersonFov == 1.0f);
}

void testFollowModeSuppressesActiveCameraId() {
    EditorStateExt project  = makeTestState(200);
    project.cameras[0].id   = "camera_1";
    project.cameras[0].name = "Camera 1";
    project.cameras[0].clips.push_back(makeClip("clip_1", 0, 200));

    CameraKeyframe tpKey;
    tpKey.mode              = CameraKeyframeMode::ThirdPerson;
    tpKey.thirdPersonCustom = true;
    project.cameras[0].keysByTick.emplace(0, tpKey);

    playback::keyframe::CameraTimelineEvaluator evaluator(project, std::string("camera_1"));
    CHECK(evaluator.shotAt(0).camera != nullptr);
    CHECK(evaluator.shotAt(0).camera->id == "camera_1");
    CHECK(evaluator.shotAt(40).camera != nullptr);
    CHECK(evaluator.shotAt(40).camera->id == "camera_1");

    // Follow mode owns the viewport: activeCameraId stays empty even when a camera track covers the playhead.
    CHECK(playback::keyframe::resolveActiveCameraId(true, &evaluator, 0).empty());
    CHECK(playback::keyframe::resolveActiveCameraId(true, &evaluator, 40).empty());

    // Exiting follow mode lets the covering camera track own activeCameraId again.
    CHECK(playback::keyframe::resolveActiveCameraId(false, &evaluator, 0) == "camera_1");
    CHECK(playback::keyframe::resolveActiveCameraId(false, &evaluator, 40) == "camera_1");
    CHECK(playback::keyframe::resolveActiveCameraId(false, nullptr, 0).empty());
}

} // namespace

void runTimeOfDayTests();

int main() {
    testTimelineNavigationSmartEdges();
    testTimelineNavigationExportPointJump();
    testAddKeyframeOverwriteAndUndo();
    testSetKeyframeModeAutoFillSubject();
    testSetKeyframesModeBatchSortingAndSubject();
    testCollectCameraClipCuts();
    testSelectionModelClipCutsAndBoxSelection();
    testHealClipCuts();
    testKeyframeTrackOrbitAngleWrapping();
    testSetCameraKeyframeThirdPersonSettingsMultiTurnAndClamping();
    testFollowModeSuppressesActiveCameraId();
    runTimeOfDayTests();
    if (gFailureCount != 0) {
        std::cerr << gFailureCount << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All editor unit tests passed.\n";
    return EXIT_SUCCESS;
}
