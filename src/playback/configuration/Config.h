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
    int         version                    = 1;
    std::string locateName                 = "zh_CN";
    bool        renderDiagnostics          = false;
    std::string renderDiagnosticExperiment = "off";
    bool        smoothPistonRender         = true;

    CommandStruct command;
};

} // namespace playback::configuration
