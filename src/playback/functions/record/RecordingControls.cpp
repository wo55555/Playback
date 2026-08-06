#include "RecordingControls.h"

#include "Recorder.h"
#include "playback/Playback.h"

#include <array>
#include <mutex>

namespace playback::functions {

namespace {

struct State {
    std::mutex             mutex;
    std::array<bool, 256>  pressed{};
    bool                   hudVisible{};
    bool                   captureKey{};
    RecordingControlAction pendingAction{RecordingControlAction::None};
};

State& state() {
    static State value;
    return value;
}

} // namespace

RecordingControls& RecordingControls::getInstance() {
    static RecordingControls instance;
    return instance;
}

void RecordingControls::setGameHudVisible(bool visible) {
    auto& current = state();
    std::scoped_lock lock(current.mutex);
    current.hudVisible = visible;
    if (!visible) current.pressed.fill(false);
}

void RecordingControls::resetPressedKeys() {
    auto& current = state();
    std::scoped_lock lock(current.mutex);
    current.pressed.fill(false);
}

bool RecordingControls::onKeyInput(UINT key, bool isDown, bool uiOwnsKeyboard) {
    auto& current = state();
    std::scoped_lock lock(current.mutex);
    if (key >= current.pressed.size()) return false;
    if (!isDown) {
        current.pressed[key] = false;
        return false;
    }
    if (current.captureKey || uiOwnsKeyboard || !current.hudVisible || current.pressed[key]) return false;
    current.pressed[key] = true;

    auto& config = Playback::getInstance().getConfig().recordingControls;
    if (key == config.toggleRecordingKey) {
        current.pendingAction = RecordingControlAction::ToggleRecording;
        return true;
    }
    if (key == config.togglePauseKey) {
        current.pendingAction = RecordingControlAction::TogglePause;
        return true;
    }
    return false;
}

RecordingControlAction RecordingControls::consumePendingAction() {
    auto& current = state();
    std::scoped_lock lock(current.mutex);
    auto const action = current.pendingAction;
    current.pendingAction = RecordingControlAction::None;
    return action;
}

bool RecordingControls::isGameHudVisible() const {
    auto& current = state();
    std::scoped_lock lock(current.mutex);
    return current.hudVisible;
}

bool RecordingControls::isCapturingKey() const {
    auto& current = state();
    std::scoped_lock lock(current.mutex);
    return current.captureKey;
}

void RecordingControls::setKeyCapture(bool active) {
    auto& current = state();
    std::scoped_lock lock(current.mutex);
    current.captureKey = active;
    if (active) current.pressed.fill(false);
}

} // namespace playback::functions
