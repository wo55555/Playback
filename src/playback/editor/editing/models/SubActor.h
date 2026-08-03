#pragma once

#include <string>
#include <vector>

namespace playback::editor::editing::model {

struct SubActor {
    std::string              id;
    std::string              name;
    std::vector<std::string> boundCameraIds;
};

} // namespace playback::editor::editing::model
