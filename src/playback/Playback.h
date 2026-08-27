#pragma once

#include "playback/configuration/Config.h"

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

#include <memory>

class Level;

namespace playback {

enum class PlaybackMode { Unknown, Record, Replay };

class Playback {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    Playback();
    ~Playback();

    static Playback& getInstance();

    [[nodiscard]] configuration::Config& getConfig();

    [[nodiscard]] std::set<ll::event::ListenerPtr>& getEventListeners();

    void setupCommands();

    void registerActions();

    [[nodiscard]] bool hook();

    [[nodiscard]] bool unhook();

    bool refreshMode();

    void refreshMode(Level& level);

    [[nodiscard]] PlaybackMode getMode() const;

    [[nodiscard]] bool isReplayMode() const;

public:
    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();

    bool enable();

    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};


} // namespace playback
