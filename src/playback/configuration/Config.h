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
};

struct Config {
    int         version    = 1;
    std::string locateName = "zh_CN";

    CommandStruct      command;
    RecordConfigStruct record;
};

} // namespace playback::configuration
