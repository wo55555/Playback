#pragma once

#include "SubActor.h"
#include "WorldActorSegment.h"

#include <string>
#include <vector>

namespace playback::state::editing::model {

struct WorldActor {
    std::string                    id;
    std::string                    name;
    int                            totalTicks{};
    std::vector<WorldActorSegment> segments;
    std::vector<SubActor>          subActors;
};

} // namespace playback::state::editing::model
