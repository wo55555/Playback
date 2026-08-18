#pragma once

#include "playback/configuration/Config.h"

namespace playback::runtime::command {

void registerPlaybackCommand();
void registerRecordCommand(configuration::CommandConfigStruct&);

} // namespace playback::runtime::command
