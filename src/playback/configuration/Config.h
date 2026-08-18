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

struct Config {
    int         version    = 1;
    std::string locateName = "zh_CN";

    CommandStruct command;
};

} // namespace playback::configuration
