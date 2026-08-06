#pragma once

#include <Windows.h>

#include <array>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace playback::config {

enum class RecordingOverlayPosition { TopLeft, TopRight, BottomLeft, BottomRight };

struct RecordingControlsConfig {
    UINT                         toggleRecordingKey = 'P';
    UINT                         togglePauseKey    = 'L';
    bool                         showStatusOverlay = true;
    RecordingOverlayPosition     overlayPosition    = RecordingOverlayPosition::TopLeft;

    static RecordingControlsConfig defaults();
    [[nodiscard]] bool             validate(std::string* error = nullptr) const;
    [[nodiscard]] std::string      keyDisplayName(UINT key) const;
};

struct CommandConfigStruct {
    bool        enabled;
    std::string command;
};

struct CommandStruct {
    CommandConfigStruct record = {true, "record"};
};

struct Config {
    int         version    = 2;
    std::string locateName = "zh_CN";

    CommandStruct           command;
    RecordingControlsConfig recordingControls;
};

[[nodiscard]] std::filesystem::path configPath();
[[nodiscard]] Config              load();
bool                              save(Config const& config);

} // namespace playback::config
