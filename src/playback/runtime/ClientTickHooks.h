#pragma once

#include <cstdint>
#include <optional>

namespace playback::runtime {

struct OfflineReplayTickToken {
    uint64_t id{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

struct OfflineReplayTickCompletion {
    uint64_t token{};
    int      replayTickBefore{};
    int      replayTickAfter{};
    bool     clientTickExecuted{};

    [[nodiscard]] int replayTicksAdvanced() const { return replayTickAfter - replayTickBefore; }
};

enum class OfflineReplayTickRequestResult : uint8_t { Requested, Unavailable, Busy };

[[nodiscard]] bool hookClientTick(bool enable);

[[nodiscard]] bool beginOfflineReplayTickGate();
void               endOfflineReplayTickGate();

[[nodiscard]] OfflineReplayTickRequestResult             requestOfflineReplayTick(OfflineReplayTickToken& token);
[[nodiscard]] bool                                       wasOfflineReplayTickCompleted(OfflineReplayTickToken token);
[[nodiscard]] std::optional<OfflineReplayTickCompletion> getOfflineReplayTickCompletion(OfflineReplayTickToken token);

} // namespace playback::runtime
