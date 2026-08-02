#pragma once

#include "MathTypes.h"

#include <map>
#include <string>
#include <vector>

namespace playback::refactor::editor {

enum class SubActorCategory : uint8_t {
    Default = 0,
    Players,
    Creatures,
    Entities
};

using AgentDetails = std::map<std::string, std::string>;

struct SubActor {
    std::string id;
    std::string name;
    SubActorCategory category{SubActorCategory::Default};
    Vec3 position{};
    Vec2 rotation{};
    AgentDetails agentDetails;
    std::vector<std::string> boundCameraIds;
};

} // namespace playback::refactor::editor
