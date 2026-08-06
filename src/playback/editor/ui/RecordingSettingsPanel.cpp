#include "RecordingSettingsPanel.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/functions/record/Recorder.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <Windows.h>

#include <string>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

struct PanelState {
    bool                           initialized{};
    bool                           captureRecordingKey{};
    bool                           capturePauseKey{};
    config::RecordingControlsConfig draft;
    std::string                    error;
};

PanelState& state() {
    static PanelState value;
    return value;
}

void beginIfNeeded() {
    auto& panel = state();
    if (panel.initialized) return;
    panel.draft       = Playback::getInstance().getConfig().recordingControls;
    panel.initialized = true;
    panel.error.clear();
}

bool captureKey(UINT& target, bool& capturing) {
    if (!capturing) return false;
    for (int key = 'A'; key <= 'Z'; ++key) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + (key - 'A')), false)) {
            target    = static_cast<UINT>(key);
            capturing = false;
            return true;
        }
    }
    for (int key = '0'; key <= '9'; ++key) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + (key - '0')), false)) {
            target    = static_cast<UINT>(key);
            capturing = false;
            return true;
        }
    }
    for (int key = 0; key < 24; ++key) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + key), false)) {
            target    = static_cast<UINT>(VK_F1 + key);
            capturing = false;
            return true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) capturing = false;
    return false;
}

} // namespace

void drawRecordingSettingsPanel(bool& open) {
    if (!open) {
        state().initialized = false;
        state().captureRecordingKey = false;
        state().capturePauseKey = false;
        return;
    }
    beginIfNeeded();
    auto& panel = state();
    auto& config = Playback::getInstance().getConfig().recordingControls;

    ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_Appearing);
    auto const title = "playback.recording.settings.title"_tr();
    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("playback.recording.settings.subtitle"_tr().c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("playback.recording.settings.toggleRecording"_tr().c_str());
    ImGui::SameLine(220.0f);
    if (panel.captureRecordingKey) {
        ImGui::Button("playback.recording.settings.pressKey"_tr().c_str());
        captureKey(panel.draft.toggleRecordingKey, panel.captureRecordingKey);
    } else if (ImGui::Button(panel.draft.keyDisplayName(panel.draft.toggleRecordingKey).c_str())) {
        panel.captureRecordingKey = true;
        panel.error.clear();
    }

    ImGui::TextUnformatted("playback.recording.settings.togglePause"_tr().c_str());
    ImGui::SameLine(220.0f);
    if (panel.capturePauseKey) {
        ImGui::Button("playback.recording.settings.pressKey"_tr().c_str());
        captureKey(panel.draft.togglePauseKey, panel.capturePauseKey);
    } else if (ImGui::Button(panel.draft.keyDisplayName(panel.draft.togglePauseKey).c_str())) {
        panel.capturePauseKey = true;
        panel.error.clear();
    }

    ImGui::Checkbox("playback.recording.settings.showOverlay"_tr().c_str(), &panel.draft.showStatusOverlay);
    std::string const topLeft = "playback.recording.settings.position.topLeft"_tr();
    std::string const topRight = "playback.recording.settings.position.topRight"_tr();
    std::string const bottomLeft = "playback.recording.settings.position.bottomLeft"_tr();
    std::string const bottomRight = "playback.recording.settings.position.bottomRight"_tr();
    char const* positions[] = {topLeft.c_str(), topRight.c_str(), bottomLeft.c_str(), bottomRight.c_str()};
    int position = static_cast<int>(panel.draft.overlayPosition);
    if (ImGui::Combo("playback.recording.settings.positionLabel"_tr().c_str(), &position, positions, IM_ARRAYSIZE(positions))) {
        panel.draft.overlayPosition = static_cast<config::RecordingOverlayPosition>(position);
    }

    auto& recorder = functions::Recorder::getInstance();
    if (!recorder.isActive()) {
        if (ImGui::Button("playback.recording.controls.start"_tr().c_str())) recorder.start();
    } else if (recorder.isPaused()) {
        if (ImGui::Button("playback.recording.controls.resume"_tr().c_str())) recorder.start();
        ImGui::SameLine();
        if (ImGui::Button("playback.recording.controls.stop"_tr().c_str())) recorder.stop();
    } else {
        if (ImGui::Button("playback.recording.controls.pause"_tr().c_str())) recorder.pause();
        ImGui::SameLine();
        if (ImGui::Button("playback.recording.controls.stop"_tr().c_str())) recorder.stop();
    }

    if (!panel.error.empty()) ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "%s", panel.error.c_str());
    if (ImGui::Button("恢复默认")) {
        panel.draft = config::RecordingControlsConfig::defaults();
        panel.error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("取消")) {
        open = false;
        panel.initialized = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("保存设置")) {
        std::string error;
        if (!panel.draft.validate(&error)) {
            panel.error = error == "duplicate_key"
                        ? "playback.recording.settings.error.duplicate"_tr()
                        : "playback.recording.settings.error.unsupported"_tr();
        } else {
            config = panel.draft;
            if (config::save(Playback::getInstance().getConfig())) {
                open = false;
                panel.initialized = false;
            } else {
                panel.error = "playback.recording.settings.error.saveFailed"_tr();
            }
        }
    }
    ImGui::End();
}

} // namespace playback::editor::ui
