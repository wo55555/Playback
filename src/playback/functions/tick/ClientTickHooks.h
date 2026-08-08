#pragma once

#include <cstdint>

namespace playback::functions {

struct OfflineReplayTickToken {
    uint64_t id{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

enum class OfflineReplayTickRequestResult : uint8_t { Requested, Unavailable, Busy };

[[nodiscard]] bool hookClientTick(bool enable);

[[nodiscard]] bool beginOfflineReplayTickGate();
void               endOfflineReplayTickGate();

[[nodiscard]] OfflineReplayTickRequestResult requestOfflineReplayTick(OfflineReplayTickToken& token);
[[nodiscard]] bool                           wasOfflineReplayTickCompleted(OfflineReplayTickToken token);

} // namespace playback::functions
