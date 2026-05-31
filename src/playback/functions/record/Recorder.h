#pragma once

#include <atomic>

namespace playback::functions {

class Recorder {
public:
    [[nodiscard]] bool isPaused() const { return mIsPaused; }

    void start();
    void pause();
    void stop();

private:
    Recorder() = default;

    std::atomic_bool mIsPaused  = false;
    std::atomic_bool mWasPaused = false;

public:
    [[nodiscard]] static Recorder& getInstance() {
        static Recorder instance;
        return instance;
    }
};

void hookNetwork(bool);

} // namespace playback::functions
