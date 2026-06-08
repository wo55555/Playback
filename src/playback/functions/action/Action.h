#pragma once

#include "playback/functions/replay/ReplaySession.h"

#include <string_view>

namespace playback::functions {

class Action {
public:
    virtual ~Action() = default;

    [[nodiscard]] virtual std::string_view name() const                                         = 0;
    virtual void                           handle(functions::ReplaySession replaySession) const = 0;
};

class ActionNextTick : Action {
public:
    [[nodiscard]] std::string_view name() const override { return "action/next_tick"; };
    void                           handle(functions::ReplaySession replaySession) const override;
};

inline const ActionNextTick ACTIION_NEXT_TICK;

} // namespace playback::functions
