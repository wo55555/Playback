#pragma once

#include <string>

namespace playback::configuration {

struct CommandConfigStruct {
    bool        enabled;
    std::string command;
};

struct CommandStruct {
    CommandConfigStruct record = {true, "record"};
};

struct RecordConfigStruct {
    // Copies the server's downloaded resource packs into the replay so it plays back anywhere.
    bool embedServerPacks = true;
    // Local view samples per second, 20 to 120 in steps of 20; 20 records only the per-tick view.
    int localPlayerUpdatesPerSecond = 20;
};

// Rounds to the nearest supported rate.
constexpr int clampLocalPlayerUpdatesPerSecond(int rate) {
    int const step = (rate + 10) / 20 * 20;
    return step < 20 ? 20 : step > 120 ? 120 : step;
}

struct Config {
    int         version    = 1;
    std::string locateName = "zh_CN";

    CommandStruct      command;
    RecordConfigStruct record;
};

} // namespace playback::configuration
