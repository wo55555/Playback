#include "playback/keyframe/TimeOfDayTimelineEvaluator.h"
#include "playback/keyframe/WeatherTimelineEvaluator.h"
#include "playback/state/editing/ClipOps.h"
#include "playback/state/editing/ProjectSerializer.h"
#include "playback/state/editing/commands/TimeOfDayCommands.h"
#include "playback/state/editing/commands/WeatherCommands.h"
#include "playback/state/editing/models/SelectionModel.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace playback::keyframe;
using namespace playback::state::editing;
using namespace playback::state::editing::command;
using namespace playback::state::editing::model;

int gFailureCount = 0;

void check(bool condition, char const* expr, int line) {
    if (condition) return;
    std::cerr << "FAIL line " << line << ": " << expr << '\n';
    ++gFailureCount;
}

bool approxEqual(double a, double b, double eps = 1e-3) { return std::abs(a - b) <= eps; }

#define CHECK(cond) check((cond), #cond, __LINE__)

EditorStateExt makeTestState(int totalTicks = 200) {
    EditorStateExt state;
    state.totalTicks = totalTicks;
    CameraEntity cam;
    cam.id   = "cam_1";
    cam.name = "Camera 1";
    state.cameras.push_back(cam);
    ClipOps::ensureDefaultScene(state, "Scene 1", "Clip 1");
    return state;
}

void testEvaluateTimeOfDayAt() {
    TimeOfDayTrack track;
    CHECK(!evaluateTimeOfDayAt(track, 0.0).has_value());

    track.keysByTick.emplace(20, TimeOfDayKeyframe{1000, CameraInterpolationType::Linear});
    track.enabled = false;
    CHECK(!evaluateTimeOfDayAt(track, 20.0).has_value());

    track.enabled = true;
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 0.0), 1000.0));
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 20.0), 1000.0));
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 100.0), 1000.0));

    // Step / Hold
    track.keysByTick.at(20).interpolationType = CameraInterpolationType::Hold;
    track.keysByTick.emplace(60, TimeOfDayKeyframe{13000, CameraInterpolationType::Linear});
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 10.0), 1000.0));
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 40.0), 1000.0));
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 59.9), 1000.0));
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 60.0), 13000.0));
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 90.0), 13000.0));

    // Linear (including multi-day ramp)
    track.keysByTick.clear();
    track.keysByTick.emplace(0, TimeOfDayKeyframe{0, CameraInterpolationType::Linear});
    track.keysByTick.emplace(100, TimeOfDayKeyframe{48000, CameraInterpolationType::Linear});
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 50.0), 24000.0));
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 25.0), 12000.0));

    // EaseIn / EaseOut / EaseInOut
    track.keysByTick.clear();
    track.keysByTick.emplace(0, TimeOfDayKeyframe{0, CameraInterpolationType::EaseIn});
    track.keysByTick.emplace(100, TimeOfDayKeyframe{8000, CameraInterpolationType::Linear});
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 50.0), 1000.0)); // 0.5^3 * 8000 = 1000

    track.keysByTick.at(0).interpolationType = CameraInterpolationType::EaseOut;
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 50.0), 7000.0)); // (1 - 0.5^3) * 8000 = 7000

    track.keysByTick.at(0).interpolationType = CameraInterpolationType::EaseInOut;
    CHECK(approxEqual(*evaluateTimeOfDayAt(track, 50.0), 4000.0));
    CHECK(*evaluateTimeOfDayAt(track, 25.0) < 2000.0);
    CHECK(*evaluateTimeOfDayAt(track, 75.0) > 6000.0);

    // CubicBezier
    track.keysByTick.clear();
    TimeOfDayKeyframe bezierKey;
    bezierKey.timeOfDay         = 0;
    bezierKey.interpolationType = CameraInterpolationType::CubicBezier;
    bezierKey.bezierCtrl1       = {0.0f, 1.0f};
    bezierKey.bezierCtrl2       = {0.0f, 1.0f};
    track.keysByTick.emplace(0, bezierKey);
    track.keysByTick.emplace(100, TimeOfDayKeyframe{10000, CameraInterpolationType::Linear});
    CHECK(*evaluateTimeOfDayAt(track, 50.0) > 5000.0);

    // Smooth (4-point Catmull-Rom) and clamping to [MinTimeOfDayTicks, MaxTimeOfDayTicks]
    track.keysByTick.clear();
    track.keysByTick.emplace(0, TimeOfDayKeyframe{6000, CameraInterpolationType::Smooth});
    track.keysByTick.emplace(50, TimeOfDayKeyframe{0, CameraInterpolationType::Smooth});
    track.keysByTick.emplace(100, TimeOfDayKeyframe{0, CameraInterpolationType::Smooth});
    track.keysByTick.emplace(150, TimeOfDayKeyframe{6000, CameraInterpolationType::Smooth});
    auto const catmullMid = evaluateTimeOfDayAt(track, 75.0);
    CHECK(catmullMid.has_value());
    CHECK(*catmullMid >= static_cast<double>(MinTimeOfDayTicks));
    CHECK(*catmullMid <= static_cast<double>(MaxTimeOfDayTicks));
}

void testResolveEffectiveTimeOfDayPrecedence() {
    EditorStateExt state = makeTestState(200);

    // 3. Neither active track nor daytime override -> nullopt
    CHECK(!resolveEffectiveTimeOfDay(state, 50.0).has_value());

    // 2. Daytime override enabled -> override value
    state.daytimeOverrideEnabled = true;
    state.daytimeOverrideTicks   = 18000;
    CHECK(approxEqual(*resolveEffectiveTimeOfDay(state, 50.0), 18000.0));

    // Disabled track still falls back to daytime override
    state.timeOfDayTrack.enabled = false;
    state.timeOfDayTrack.keysByTick.emplace(0, TimeOfDayKeyframe{1000, CameraInterpolationType::Linear});
    CHECK(!hasActiveTimeOfDayTrack(state));
    CHECK(approxEqual(*resolveEffectiveTimeOfDay(state, 50.0), 18000.0));

    // 1. Active TimeOfDayTrack overrides daytimeOverrideEnabled
    state.timeOfDayTrack.enabled = true;
    state.timeOfDayTrack.keysByTick.emplace(100, TimeOfDayKeyframe{13000, CameraInterpolationType::Linear});
    CHECK(hasActiveTimeOfDayTrack(state));
    CHECK(approxEqual(*resolveEffectiveTimeOfDay(state, 50.0), 7000.0));
}

void testTimeOfDayCommandsAndOverwriteUndoRedo() {
    EditorStateExt state = makeTestState(200);

    AddTimeOfDayKeyframe addCmd(40, 1000, CameraInterpolationType::Smooth);
    addCmd.execute(state);
    CHECK(addCmd.didChange());
    CHECK(addCmd.label() == "playback.refactorEditor.history.addTimeOfDayKeyframe");
    CHECK(state.timeOfDayTrack.present);
    CHECK(state.timeOfDayTrack.keysByTick.size() == 1);
    CHECK(state.timeOfDayTrack.keysByTick.at(40).timeOfDay == 1000);
    CHECK(state.timeOfDayTrack.keysByTick.at(40).interpolationType == CameraInterpolationType::Smooth);

    SetTimeOfDayKeyframeBezier bezierCmd(40, {0.1f, 0.9f}, {0.2f, 0.8f});
    bezierCmd.execute(state);
    CHECK(bezierCmd.didChange());

    // Overwriting with default Linear preserves existing interpolation & bezier controls.
    AddTimeOfDayKeyframe replaceCmd(40, 13000);
    replaceCmd.execute(state);
    CHECK(replaceCmd.didChange());
    CHECK(replaceCmd.label() == "playback.refactorEditor.history.replaceTimeOfDayKeyframe");
    CHECK(state.timeOfDayTrack.keysByTick.at(40).timeOfDay == 13000);
    CHECK(state.timeOfDayTrack.keysByTick.at(40).interpolationType == CameraInterpolationType::Smooth);
    CHECK(approxEqual(state.timeOfDayTrack.keysByTick.at(40).bezierCtrl1.x, 0.1f, 1e-5));
    CHECK(approxEqual(state.timeOfDayTrack.keysByTick.at(40).bezierCtrl2.y, 0.8f, 1e-5));

    replaceCmd.undo(state);
    CHECK(state.timeOfDayTrack.keysByTick.at(40).timeOfDay == 1000);
    CHECK(state.timeOfDayTrack.keysByTick.at(40).interpolationType == CameraInterpolationType::Smooth);

    replaceCmd.execute(state);
    CHECK(state.timeOfDayTrack.keysByTick.at(40).timeOfDay == 13000);

    // Move keyframe
    MoveTimeOfDayKeyframe moveCmd(40, 80);
    moveCmd.execute(state);
    CHECK(moveCmd.didChange());
    CHECK(!state.timeOfDayTrack.keysByTick.contains(40));
    CHECK(state.timeOfDayTrack.keysByTick.contains(80));
    CHECK(state.timeOfDayTrack.keysByTick.at(80).timeOfDay == 13000);

    moveCmd.undo(state);
    CHECK(state.timeOfDayTrack.keysByTick.contains(40));
    CHECK(!state.timeOfDayTrack.keysByTick.contains(80));

    // Locked track blocks mutations
    SetTimeOfDayTrackLocked lockCmd(true);
    lockCmd.execute(state);
    CHECK(lockCmd.didChange());
    AddTimeOfDayKeyframe lockedAdd(90, 6000);
    lockedAdd.execute(state);
    CHECK(!lockedAdd.didChange());
    lockCmd.undo(state);
    CHECK(!state.timeOfDayTrack.locked);

    // Scene daytime & weather overrides
    SetSceneDaytimeOverride daytimeCmd(true, 999999);
    daytimeCmd.execute(state);
    CHECK(daytimeCmd.didChange());
    CHECK(state.daytimeOverrideEnabled);
    CHECK(state.daytimeOverrideTicks == MaxTimeOfDayTicks);
    CHECK(state.scenes[0].daytimeOverrideEnabled);
    CHECK(state.scenes[0].daytimeOverrideTicks == MaxTimeOfDayTicks);

    SetSceneWeatherOverride weatherCmd(WeatherOverride::Thunder);
    weatherCmd.execute(state);
    CHECK(weatherCmd.didChange());
    CHECK(state.weatherOverride == WeatherOverride::Thunder);
    CHECK(state.scenes[0].weatherOverride == WeatherOverride::Thunder);

    weatherCmd.undo(state);
    CHECK(state.weatherOverride == WeatherOverride::Recorded);
    daytimeCmd.undo(state);
    CHECK(!state.daytimeOverrideEnabled);
    CHECK(state.daytimeOverrideTicks == 6000);
}

void testCopySceneScopeFieldsAndSelectionModel() {
    EditorStateExt state         = makeTestState(200);
    state.timeOfDayTrack.present = true;
    state.timeOfDayTrack.keysByTick.emplace(10, TimeOfDayKeyframe{1000, CameraInterpolationType::Linear});
    state.timeOfDayTrack.keysByTick.emplace(50, TimeOfDayKeyframe{12000, CameraInterpolationType::EaseOut});
    state.daytimeOverrideEnabled = true;
    state.daytimeOverrideTicks   = 18000;
    state.weatherOverride        = WeatherOverride::Rain;
    ClipOps::syncActiveSceneFromWorking(state);

    Scene second;
    second.id                     = "scene_2";
    second.name                   = "Scene 2";
    second.daytimeOverrideEnabled = false;
    second.daytimeOverrideTicks   = 0;
    second.weatherOverride        = WeatherOverride::Snow;
    state.scenes.push_back(second);

    auto const built = ClipOps::buildProjectForScene(state, state.scenes[1]);
    CHECK(!built.daytimeOverrideEnabled);
    CHECK(built.daytimeOverrideTicks == 0);
    CHECK(built.weatherOverride == WeatherOverride::Snow);
    CHECK(built.timeOfDayTrack.keysByTick.empty());

    state.activeSceneId = "scene_2";
    ClipOps::loadWorkingFromActiveScene(state);
    CHECK(!state.daytimeOverrideEnabled);
    CHECK(state.weatherOverride == WeatherOverride::Snow);

    state.activeSceneId = "scene_1";
    ClipOps::loadWorkingFromActiveScene(state);
    CHECK(state.daytimeOverrideEnabled);
    CHECK(state.daytimeOverrideTicks == 18000);
    CHECK(state.weatherOverride == WeatherOverride::Rain);
    CHECK(state.timeOfDayTrack.keysByTick.size() == 2);

    // SelectionModel multi-selection & pruning
    SelectionModel selection;
    selection.selectTimeOfDayKeyframes({50, 10, 50});
    CHECK(selection.isTimeOfDayKeyframeSelected(10));
    CHECK(selection.isTimeOfDayKeyframeSelected(50));
    CHECK(selection.selectedTimeOfDayKeyframes() == (std::vector<int>{10, 50}));

    state.timeOfDayTrack.keysByTick.erase(10);
    selection.pruneInvalid(state);
    CHECK(!selection.isTimeOfDayKeyframeSelected(10));
    CHECK(selection.isTimeOfDayKeyframeSelected(50));
    CHECK(selection.selectedTimeOfDayKeyframes() == (std::vector<int>{50}));

    state.timeOfDayTrack.keysByTick.erase(50);
    selection.pruneInvalid(state);
    CHECK(!selection.hasSelection());
}

void testProjectSerializerRoundTrip() {
    EditorStateExt project         = makeTestState(300);
    project.projectName            = "tod-test";
    project.timeOfDayTrack.present = true;
    project.timeOfDayTrack.enabled = false;
    project.timeOfDayTrack.locked  = true;

    TimeOfDayKeyframe k1;
    k1.timeOfDay         = 0;
    k1.interpolationType = CameraInterpolationType::CubicBezier;
    k1.bezierCtrl1       = {0.25f, 0.1f};
    k1.bezierCtrl2       = {0.75f, 0.9f};
    project.timeOfDayTrack.keysByTick.emplace(0, k1);
    project.timeOfDayTrack.keysByTick.emplace(150, TimeOfDayKeyframe{48000, CameraInterpolationType::Smooth});

    project.daytimeOverrideEnabled = true;
    project.daytimeOverrideTicks   = 12000;
    project.weatherOverride        = WeatherOverride::Overcast;
    ClipOps::syncActiveSceneFromWorking(project);

    Scene scene2;
    scene2.id                     = "scene_2";
    scene2.name                   = "Night Scene";
    scene2.daytimeOverrideEnabled = true;
    scene2.daytimeOverrideTicks   = 18000;
    scene2.weatherOverride        = WeatherOverride::Snow;
    project.scenes.push_back(scene2);

    std::string const jsonText = serializeProject(project);
    CHECK(jsonText.find("\"overcast\"") != std::string::npos);
    CHECK(jsonText.find("\"snow\"") != std::string::npos);

    EditorStateExt loaded;
    std::string    error;
    CHECK(deserializeProject(jsonText, loaded, error));
    CHECK(error.empty());
    CHECK(loaded.scenes.size() == 2);
    CHECK(loaded.timeOfDayTrack.present);
    CHECK(!loaded.timeOfDayTrack.enabled);
    CHECK(loaded.timeOfDayTrack.locked);
    CHECK(loaded.timeOfDayTrack.keysByTick.size() == 2);
    CHECK(loaded.timeOfDayTrack.keysByTick.at(0).interpolationType == CameraInterpolationType::CubicBezier);
    CHECK(approxEqual(loaded.timeOfDayTrack.keysByTick.at(0).bezierCtrl1.x, 0.25f, 1e-5));
    CHECK(loaded.timeOfDayTrack.keysByTick.at(150).timeOfDay == 48000);
    CHECK(loaded.daytimeOverrideEnabled);
    CHECK(loaded.daytimeOverrideTicks == 12000);
    CHECK(loaded.weatherOverride == WeatherOverride::Overcast);

    CHECK(loaded.scenes[1].daytimeOverrideEnabled);
    CHECK(loaded.scenes[1].daytimeOverrideTicks == 18000);
    CHECK(loaded.scenes[1].weatherOverride == WeatherOverride::Snow);

    // Top-level legacy single-scene fallback
    std::string const legacyPayload = R"({
        "formatVersion": 1,
        "totalTicks": 100,
        "daytimeOverrideEnabled": true,
        "daytimeOverrideTicks": 13000,
        "weatherOverride": "thunder",
        "timeOfDayTrack": {
            "present": true,
            "enabled": true,
            "locked": false,
            "keys": [
                { "tick": 10, "timeOfDay": 6000, "interpolationType": 1 }
            ]
        }
    })";
    EditorStateExt    legacyLoaded;
    CHECK(deserializeProject(legacyPayload, legacyLoaded, error));
    CHECK(legacyLoaded.daytimeOverrideEnabled);
    CHECK(legacyLoaded.daytimeOverrideTicks == 13000);
    CHECK(legacyLoaded.weatherOverride == WeatherOverride::Thunder);
    CHECK(legacyLoaded.timeOfDayTrack.keysByTick.size() == 1);
    CHECK(legacyLoaded.timeOfDayTrack.keysByTick.at(10).timeOfDay == 6000);
}

void testEvaluateAndResolveWeatherAt() {
    WeatherTrack track;
    CHECK(!hasActiveWeatherTrack(track));
    CHECK(!evaluateWeatherAt(track, 0.0).has_value());

    track.keysByTick.emplace(20, WeatherKeyframe{WeatherOverride::Rain});
    track.keysByTick.emplace(60, WeatherKeyframe{WeatherOverride::Thunder});
    track.keysByTick.emplace(100, WeatherKeyframe{WeatherOverride::Recorded});

    // Disabled track returns nullopt
    track.enabled = false;
    CHECK(!hasActiveWeatherTrack(track));
    CHECK(!evaluateWeatherAt(track, 20.0).has_value());
    CHECK(!evaluateWeatherAt(track, 80.0).has_value());

    // Enabled track: discrete step evaluation with floor(tick)
    track.enabled = true;
    CHECK(hasActiveWeatherTrack(track));
    CHECK(!evaluateWeatherAt(track, 0.0).has_value());
    CHECK(!evaluateWeatherAt(track, 19.99).has_value());
    CHECK(!evaluateWeatherAt(track, std::numeric_limits<double>::quiet_NaN()).has_value());
    CHECK(!evaluateWeatherAt(track, std::numeric_limits<double>::infinity()).has_value());

    CHECK(evaluateWeatherAt(track, 20.0) == WeatherOverride::Rain);
    CHECK(evaluateWeatherAt(track, 59.99) == WeatherOverride::Rain);
    CHECK(evaluateWeatherAt(track, 60.0) == WeatherOverride::Thunder);
    CHECK(evaluateWeatherAt(track, 99.9) == WeatherOverride::Thunder);
    CHECK(evaluateWeatherAt(track, 100.0) == WeatherOverride::Recorded);
    CHECK(evaluateWeatherAt(track, 5000.0) == WeatherOverride::Recorded);

    // Precedence against project.weatherOverride
    EditorStateExt state  = makeTestState(200);
    state.weatherOverride = WeatherOverride::Overcast;
    CHECK(resolveEffectiveWeather(state, 10.0) == WeatherOverride::Overcast);

    state.weatherTrack = track;
    CHECK(hasActiveWeatherTrack(state));
    // Before first keyframe falls back to scene weatherOverride
    CHECK(resolveEffectiveWeather(state, 10.0) == WeatherOverride::Overcast);
    // At/after keyframes overrides scene weatherOverride, including Recorded
    CHECK(resolveEffectiveWeather(state, 20.0) == WeatherOverride::Rain);
    CHECK(resolveEffectiveWeather(state, 60.0) == WeatherOverride::Thunder);
    CHECK(resolveEffectiveWeather(state, 100.0) == WeatherOverride::Recorded);

    // Disabled track falls back to scene weatherOverride
    state.weatherTrack.enabled = false;
    CHECK(!hasActiveWeatherTrack(state));
    CHECK(resolveEffectiveWeather(state, 60.0) == WeatherOverride::Overcast);
}

void testWeatherCommandsAndUndoRedo() {
    EditorStateExt state = makeTestState(200);

    // Add & remove track
    AddWeatherTrack addTrack;
    addTrack.execute(state);
    CHECK(addTrack.didChange());
    CHECK(addTrack.label() == "playback.refactorEditor.history.addWeatherTrack");
    CHECK(state.weatherTrack.present);
    CHECK(state.weatherTrack.isVisible());

    AddWeatherTrack addTrackAgain;
    addTrackAgain.execute(state);
    CHECK(!addTrackAgain.didChange());

    // Add keyframe
    AddWeatherKeyframe addKey1(40, WeatherOverride::Rain);
    addKey1.execute(state);
    CHECK(addKey1.didChange());
    CHECK(addKey1.label() == "playback.refactorEditor.history.addWeatherKeyframe");
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Rain);

    // Same tick, same weather -> no-op
    AddWeatherKeyframe addSame(40, WeatherOverride::Rain);
    addSame.execute(state);
    CHECK(!addSame.didChange());

    // Same tick, different weather -> updateWeatherKeyframe
    AddWeatherKeyframe updateKey(40, WeatherOverride::Thunder);
    updateKey.execute(state);
    CHECK(updateKey.didChange());
    CHECK(updateKey.label() == "playback.refactorEditor.history.updateWeatherKeyframe");
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Thunder);

    updateKey.undo(state);
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Rain);
    updateKey.execute(state);
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Thunder);

    // Set single value
    SetWeatherKeyframeValue setVal(40, WeatherOverride::Snow);
    setVal.execute(state);
    CHECK(setVal.didChange());
    CHECK(setVal.label() == "playback.refactorEditor.history.setWeatherKeyframeValue");
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Snow);
    setVal.undo(state);
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Thunder);
    setVal.execute(state);

    // Add second keyframe and test batch value edit
    AddWeatherKeyframe addKey2(80, WeatherOverride::Clear);
    addKey2.execute(state);
    CHECK(addKey2.didChange());

    SetWeatherKeyframesValue setMultiVal({40, 80, 999}, WeatherOverride::Overcast);
    setMultiVal.execute(state);
    CHECK(setMultiVal.didChange());
    CHECK(setMultiVal.label() == "playback.refactorEditor.history.setWeatherKeyframesValue");
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Overcast);
    CHECK(state.weatherTrack.keysByTick.at(80).weather == WeatherOverride::Overcast);
    setMultiVal.undo(state);
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Snow);
    CHECK(state.weatherTrack.keysByTick.at(80).weather == WeatherOverride::Clear);

    // Move single keyframe
    MoveWeatherKeyframe moveSingle(40, 50);
    moveSingle.execute(state);
    CHECK(moveSingle.didChange());
    CHECK(moveSingle.label() == "playback.refactorEditor.history.moveWeatherKeyframe");
    CHECK(!state.weatherTrack.keysByTick.contains(40));
    CHECK(state.weatherTrack.keysByTick.at(50).weather == WeatherOverride::Snow);

    // Move onto existing tick is rejected
    MoveWeatherKeyframe moveCollide(50, 80);
    moveCollide.execute(state);
    CHECK(!moveCollide.didChange());

    moveSingle.undo(state);
    CHECK(state.weatherTrack.keysByTick.contains(40));
    CHECK(!state.weatherTrack.keysByTick.contains(50));

    // Move multiple keyframes (including overlapping destination within moving set)
    AddWeatherKeyframe addKey3(60, WeatherOverride::Recorded);
    addKey3.execute(state);
    MoveWeatherKeyframes moveMulti({40, 60}, 20);
    moveMulti.execute(state);
    CHECK(!moveMulti.didChange()); // 60+20=80 collides with non-moving key at 80

    MoveWeatherKeyframes moveMultiOk({40, 60}, 10);
    moveMultiOk.execute(state);
    CHECK(moveMultiOk.didChange());
    CHECK(moveMultiOk.label() == "playback.refactorEditor.history.moveWeatherKeyframes");
    CHECK(!state.weatherTrack.keysByTick.contains(40));
    CHECK(state.weatherTrack.keysByTick.at(50).weather == WeatherOverride::Snow);
    CHECK(state.weatherTrack.keysByTick.at(70).weather == WeatherOverride::Recorded);
    moveMultiOk.undo(state);
    CHECK(state.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Snow);
    CHECK(state.weatherTrack.keysByTick.at(60).weather == WeatherOverride::Recorded);

    // Lock protection
    SetWeatherTrackLocked lockCmd(true);
    lockCmd.execute(state);
    CHECK(lockCmd.didChange());
    CHECK(lockCmd.label() == "playback.refactorEditor.history.setWeatherTrackLocked");

    CHECK([&] {
        AddWeatherKeyframe c(100, WeatherOverride::Clear);
        c.execute(state);
        return !c.didChange();
    }());
    CHECK([&] {
        RemoveWeatherKeyframe c(40);
        c.execute(state);
        return !c.didChange();
    }());
    CHECK([&] {
        RemoveWeatherKeyframes c({40, 60});
        c.execute(state);
        return !c.didChange();
    }());
    CHECK([&] {
        MoveWeatherKeyframe c(40, 45);
        c.execute(state);
        return !c.didChange();
    }());
    CHECK([&] {
        MoveWeatherKeyframes c({40, 60}, 5);
        c.execute(state);
        return !c.didChange();
    }());
    CHECK([&] {
        SetWeatherKeyframeValue c(40, WeatherOverride::Clear);
        c.execute(state);
        return !c.didChange();
    }());
    CHECK([&] {
        SetWeatherKeyframesValue c({40, 60}, WeatherOverride::Clear);
        c.execute(state);
        return !c.didChange();
    }());
    CHECK([&] {
        RemoveWeatherTrack c;
        c.execute(state);
        return !c.didChange();
    }());

    // SetWeatherTrackEnabled remains allowed while locked (matching TimeOfDayTrack)
    SetWeatherTrackEnabled disableCmd(false);
    disableCmd.execute(state);
    CHECK(disableCmd.didChange());
    CHECK(disableCmd.label() == "playback.refactorEditor.history.setWeatherTrackEnabled");
    CHECK(!state.weatherTrack.enabled);
    disableCmd.undo(state);
    CHECK(state.weatherTrack.enabled);

    lockCmd.undo(state);
    CHECK(!state.weatherTrack.locked);

    // Remove single & batch keyframes
    RemoveWeatherKeyframe removeOne(80);
    removeOne.execute(state);
    CHECK(removeOne.didChange());
    CHECK(removeOne.label() == "playback.refactorEditor.history.removeWeatherKeyframe");
    CHECK(!state.weatherTrack.keysByTick.contains(80));
    removeOne.undo(state);
    CHECK(state.weatherTrack.keysByTick.contains(80));

    RemoveWeatherKeyframes removeMany({40, 60});
    removeMany.execute(state);
    CHECK(removeMany.didChange());
    CHECK(removeMany.label() == "playback.refactorEditor.history.removeWeatherKeyframes");
    CHECK(state.weatherTrack.keysByTick.size() == 1);
    removeMany.undo(state);
    CHECK(state.weatherTrack.keysByTick.size() == 3);

    // Remove track
    RemoveWeatherTrack removeTrack;
    removeTrack.execute(state);
    CHECK(removeTrack.didChange());
    CHECK(removeTrack.label() == "playback.refactorEditor.history.removeWeatherTrack");
    CHECK(!state.weatherTrack.isVisible());
    CHECK(state.weatherTrack.keysByTick.empty());
    removeTrack.undo(state);
    CHECK(state.weatherTrack.isVisible());
    CHECK(state.weatherTrack.keysByTick.size() == 3);
}

void testWeatherSelectionAndSceneScope() {
    EditorStateExt state       = makeTestState(200);
    state.weatherTrack.present = true;
    state.weatherTrack.keysByTick.emplace(10, WeatherKeyframe{WeatherOverride::Clear});
    state.weatherTrack.keysByTick.emplace(50, WeatherKeyframe{WeatherOverride::Rain});
    ClipOps::syncActiveSceneFromWorking(state);

    Scene second;
    second.id                   = "scene_2";
    second.name                 = "Scene 2";
    second.weatherTrack.present = true;
    second.weatherTrack.keysByTick.emplace(30, WeatherKeyframe{WeatherOverride::Snow});
    state.scenes.push_back(second);

    auto const built = ClipOps::buildProjectForScene(state, state.scenes[1]);
    CHECK(built.weatherTrack.keysByTick.size() == 1);
    CHECK(built.weatherTrack.keysByTick.at(30).weather == WeatherOverride::Snow);

    state.activeSceneId = "scene_2";
    ClipOps::loadWorkingFromActiveScene(state);
    CHECK(state.weatherTrack.keysByTick.size() == 1);
    CHECK(state.weatherTrack.keysByTick.at(30).weather == WeatherOverride::Snow);

    state.activeSceneId = "scene_1";
    ClipOps::loadWorkingFromActiveScene(state);
    CHECK(state.weatherTrack.keysByTick.size() == 2);

    SelectionModel selection;
    selection.selectWeatherTrack();
    CHECK(selection.isWeatherTrackSelected());
    CHECK(selection.selectedIds() == (std::vector<std::string>{"weather_track"}));

    selection.selectWeatherKeyframe(10);
    CHECK(selection.selectedWeatherKeyframe() == 10);
    CHECK(selection.isWeatherKeyframeSelected(10));
    CHECK(selection.selectedWeatherKeyframes() == (std::vector<int>{10}));

    selection.selectWeatherKeyframes({50, 10, 50});
    CHECK(selection.isWeatherKeyframeSelected(10));
    CHECK(selection.isWeatherKeyframeSelected(50));
    CHECK(selection.selectedWeatherKeyframes() == (std::vector<int>{10, 50}));

    state.weatherTrack.keysByTick.erase(10);
    selection.pruneInvalid(state);
    CHECK(!selection.isWeatherKeyframeSelected(10));
    CHECK(selection.isWeatherKeyframeSelected(50));
    CHECK(selection.selectedWeatherKeyframes() == (std::vector<int>{50}));

    state.weatherTrack.keysByTick.erase(50);
    selection.pruneInvalid(state);
    CHECK(!selection.hasSelection());

    selection.selectWeatherTrack();
    CHECK(selection.isWeatherTrackSelected());
    state.weatherTrack.present = false;
    selection.pruneInvalid(state);
    CHECK(!selection.hasSelection());
}

void testWeatherProjectSerializerRoundTrip() {
    // Hidden default track is omitted
    EditorStateExt    emptyProject = makeTestState(200);
    std::string const emptyJson    = serializeProject(emptyProject);
    CHECK(emptyJson.find("\"weatherTrack\"") == std::string::npos);

    // Visible track with keys round-trips across scenes
    EditorStateExt project       = makeTestState(300);
    project.weatherTrack.present = true;
    project.weatherTrack.enabled = false;
    project.weatherTrack.locked  = true;
    project.weatherTrack.keysByTick.emplace(0, WeatherKeyframe{WeatherOverride::Clear});
    project.weatherTrack.keysByTick.emplace(20, WeatherKeyframe{WeatherOverride::Overcast});
    project.weatherTrack.keysByTick.emplace(40, WeatherKeyframe{WeatherOverride::Rain});
    project.weatherTrack.keysByTick.emplace(60, WeatherKeyframe{WeatherOverride::Snow});
    project.weatherTrack.keysByTick.emplace(80, WeatherKeyframe{WeatherOverride::Thunder});
    project.weatherTrack.keysByTick.emplace(100, WeatherKeyframe{WeatherOverride::Recorded});
    ClipOps::syncActiveSceneFromWorking(project);

    Scene scene2;
    scene2.id                   = "scene_2";
    scene2.name                 = "Scene 2";
    scene2.weatherTrack.present = true;
    project.scenes.push_back(scene2);

    std::string const jsonText = serializeProject(project);
    CHECK(jsonText.find("\"weatherTrack\"") != std::string::npos);

    EditorStateExt loaded;
    std::string    error;
    CHECK(deserializeProject(jsonText, loaded, error));
    CHECK(error.empty());
    CHECK(loaded.scenes.size() == 2);
    CHECK(loaded.weatherTrack.present);
    CHECK(!loaded.weatherTrack.enabled);
    CHECK(loaded.weatherTrack.locked);
    CHECK(loaded.weatherTrack.keysByTick.size() == 6);
    CHECK(loaded.weatherTrack.keysByTick.at(0).weather == WeatherOverride::Clear);
    CHECK(loaded.weatherTrack.keysByTick.at(20).weather == WeatherOverride::Overcast);
    CHECK(loaded.weatherTrack.keysByTick.at(40).weather == WeatherOverride::Rain);
    CHECK(loaded.weatherTrack.keysByTick.at(60).weather == WeatherOverride::Snow);
    CHECK(loaded.weatherTrack.keysByTick.at(80).weather == WeatherOverride::Thunder);
    CHECK(loaded.weatherTrack.keysByTick.at(100).weather == WeatherOverride::Recorded);

    CHECK(loaded.scenes[1].weatherTrack.present);
    CHECK(loaded.scenes[1].weatherTrack.enabled);
    CHECK(!loaded.scenes[1].weatherTrack.locked);
    CHECK(loaded.scenes[1].weatherTrack.keysByTick.empty());

    // Top-level legacy single-scene fallback
    std::string const legacyPayload = R"({
        "formatVersion": 1,
        "totalTicks": 100,
        "weatherTrack": {
            "present": true,
            "enabled": true,
            "locked": false,
            "keys": [
                { "tick": 15, "weather": "rain" }
            ]
        }
    })";
    EditorStateExt    legacyLoaded;
    CHECK(deserializeProject(legacyPayload, legacyLoaded, error));
    CHECK(legacyLoaded.weatherTrack.present);
    CHECK(legacyLoaded.weatherTrack.keysByTick.size() == 1);
    CHECK(legacyLoaded.weatherTrack.keysByTick.at(15).weather == WeatherOverride::Rain);
}

} // namespace

void runTimeOfDayTests() {
    gFailureCount = 0;
    testEvaluateTimeOfDayAt();
    testResolveEffectiveTimeOfDayPrecedence();
    testTimeOfDayCommandsAndOverwriteUndoRedo();
    testCopySceneScopeFieldsAndSelectionModel();
    testProjectSerializerRoundTrip();
    testEvaluateAndResolveWeatherAt();
    testWeatherCommandsAndUndoRedo();
    testWeatherSelectionAndSceneScope();
    testWeatherProjectSerializerRoundTrip();
    if (gFailureCount != 0) {
        std::cerr << gFailureCount << " TimeOfDay test(s) failed\n";
        std::exit(EXIT_FAILURE);
    }
}

#ifdef TIMEOFDAY_TEST_MAIN
int main() {
    runTimeOfDayTests();
    std::cout << "All TimeOfDay unit tests passed.\n";
    return EXIT_SUCCESS;
}
#endif
