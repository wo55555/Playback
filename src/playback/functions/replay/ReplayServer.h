#pragma once

namespace playback::functions {

class ReplayServer {
public:
    void handleNextTick();
    void handleGamePacket();
};

} // namespace playback::functions
