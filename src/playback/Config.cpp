#include "Config.h"

#include "playback/Playback.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>

namespace playback::config {

namespace {

constexpr char kConfigPath[] = "mods/playback/config.json";

char const* positionName(RecordingOverlayPosition position) {
    switch (position) {
    case RecordingOverlayPosition::TopRight:
        return "top_right";
    case RecordingOverlayPosition::BottomLeft:
        return "bottom_left";
    case RecordingOverlayPosition::BottomRight:
        return "bottom_right";
    case RecordingOverlayPosition::TopLeft:
    default:
        return "top_left";
    }
}

RecordingOverlayPosition parsePosition(std::string_view value) {
    if (value == "top_right") return RecordingOverlayPosition::TopRight;
    if (value == "bottom_left") return RecordingOverlayPosition::BottomLeft;
    if (value == "bottom_right") return RecordingOverlayPosition::BottomRight;
    return RecordingOverlayPosition::TopLeft;
}

bool isAllowedKey(UINT key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return true;
    return key >= VK_F1 && key <= VK_F24;
}

} // namespace

RecordingControlsConfig RecordingControlsConfig::defaults() { return {}; }

bool RecordingControlsConfig::validate(std::string* error) const {
    auto fail = [&](char const* message) {
        if (error) *error = message;
        return false;
    };
    if (!isAllowedKey(toggleRecordingKey) || !isAllowedKey(togglePauseKey)) return fail("unsupported_key");
    if (toggleRecordingKey == togglePauseKey) return fail("duplicate_key");
    return true;
}

std::string RecordingControlsConfig::keyDisplayName(UINT key) const {
    if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>(key));
    if (key >= '0' && key <= '9') return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
    return {};
}

std::filesystem::path configPath() { return kConfigPath; }

Config load() {
    Config config;
    std::ifstream input(configPath());
    if (!input) return config;

    auto json = nlohmann::json::parse(input, nullptr, false);
    if (!json.is_object()) return config;

    config.version    = std::max(2, json.value("version", 1));
    config.locateName = json.value("locateName", config.locateName);
    if (auto command = json.find("command"); command != json.end() && command->is_object()) {
        if (auto record = command->find("record"); record != command->end() && record->is_object()) {
            config.command.record.enabled = record->value("enabled", config.command.record.enabled);
            config.command.record.command = record->value("command", config.command.record.command);
        }
    }
    if (auto recording = json.find("recordingControls"); recording != json.end() && recording->is_object()) {
        config.recordingControls.toggleRecordingKey = recording->value(
            "toggleRecordingKey",
            config.recordingControls.toggleRecordingKey
        );
        config.recordingControls.togglePauseKey = recording->value(
            "togglePauseKey",
            config.recordingControls.togglePauseKey
        );
        config.recordingControls.showStatusOverlay = recording->value(
            "showStatusOverlay",
            config.recordingControls.showStatusOverlay
        );
        config.recordingControls.overlayPosition = parsePosition(
            recording->value("overlayPosition", std::string(positionName(config.recordingControls.overlayPosition)))
        );
    }

    std::string error;
    if (!config.recordingControls.validate(&error)) {
        Playback::getInstance().getSelf().getLogger().warn("Invalid recording controls configuration: {}", error);
        config.recordingControls = RecordingControlsConfig::defaults();
    }
    return config;
}

bool save(Config const& config) {
    std::string error;
    if (!config.recordingControls.validate(&error)) {
        Playback::getInstance().getSelf().getLogger().error("Cannot save recording controls configuration: {}", error);
        return false;
    }

    nlohmann::ordered_json json{
        {"version", config.version},
        {"locateName", config.locateName},
        {"command", {{"record", {{"enabled", config.command.record.enabled}, {"command", config.command.record.command}}}}},
        {"recordingControls",
         {{"toggleRecordingKey", config.recordingControls.toggleRecordingKey},
          {"togglePauseKey", config.recordingControls.togglePauseKey},
          {"showStatusOverlay", config.recordingControls.showStatusOverlay},
          {"overlayPosition", positionName(config.recordingControls.overlayPosition)}}}
    };

    std::error_code errorCode;
    std::filesystem::create_directories(configPath().parent_path(), errorCode);
    if (errorCode) return false;
    std::ofstream output(configPath(), std::ios::trunc);
    if (!output) return false;
    output << json.dump(2);
    return output.good();
}

} // namespace playback::config
