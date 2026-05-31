#pragma once

#include "playback/functions/replay/ReplayServer.h"

#include <string_view>

namespace playback::action {

class Action {
public:
    virtual ~Action() = default;

    [[nodiscard]] virtual std::string_view name() const                                       = 0;
    virtual void                           handle(functions::ReplayServer replayServer) const = 0;
};

class ActionNextTick : Action {
public:
    [[nodiscard]] std::string_view name() const override { return "action/next_tick"; };
    void                           handle(functions::ReplayServer replayServer) const override;
};

class ActionGamePacket : Action {
public:
    [[nodiscard]] std::string_view name() const override { return "action/game_packet"; };
    void                           handle(functions::ReplayServer replayServer) const override;
};

inline const ActionNextTick   ACTIION_NEXT_TICK;
inline const ActionGamePacket ACTION_GAME_PACKET;

} // namespace playback::action
