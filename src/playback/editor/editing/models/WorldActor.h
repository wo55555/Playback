#pragma once

#include "SubActor.h"
#include "WorldActorSegment.h"

#include <vector>

namespace playback::editor::editing::model {

struct WorldActor {
    std::vector<WorldActorSegment> segments;
    std::vector<SubActor>          subActors;
};

} // namespace playback::editor::editing::model
