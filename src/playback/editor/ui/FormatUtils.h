#pragma once

#include <array>
#include <cstdio>
#include <string>

namespace playback::editor::utils {

inline std::string formatTimestamp(int ticks) {
    ticks             = std::max(0, ticks);
    int const seconds = ticks / 20;
    int const minutes = seconds / 60;
    int const hours   = minutes / 60;

    std::array<char, 24> buffer{};
    if (hours > 0) {
        std::snprintf(buffer.data(), buffer.size(), "%02d:%02d:%02d", hours, minutes % 60, seconds % 60);
    } else {
        std::snprintf(buffer.data(), buffer.size(), "%02d:%02d", minutes, seconds % 60);
    }
    return buffer.data();
}

} // namespace playback::editor::utils
