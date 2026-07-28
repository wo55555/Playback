#pragma once

#include "playback/Config.h"

namespace playback::command {

void registerPlaybackCommand();
void registerRecordCommand(config::CommandConfigStruct&);

} // namespace playback::command
