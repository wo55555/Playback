#pragma once

#include <optional>
#include <string_view>

enum class MinecraftPacketIds : int;

namespace playback::functions {

enum class PacketLifecycle {
    Ignore,
    PreWorldHandshake,
    SnapshotLatest,
    SnapshotSequence,
    Timeline
};

[[nodiscard]] constexpr std::string_view packetLifecycleName(PacketLifecycle lifecycle) noexcept {
    switch (lifecycle) {
    case PacketLifecycle::Ignore:
        return "Ignore";
    case PacketLifecycle::PreWorldHandshake:
        return "PreWorldHandshake";
    case PacketLifecycle::SnapshotLatest:
        return "SnapshotLatest";
    case PacketLifecycle::SnapshotSequence:
        return "SnapshotSequence";
    case PacketLifecycle::Timeline:
        return "Timeline";
    }
    return "Unknown";
}

struct PacketLifecycleSemantics {
    PacketLifecycle                   lifecycle{PacketLifecycle::Timeline};
    bool                              replayEverySnapshot{};
    bool                              startsConfigurationEpoch{};
    std::optional<MinecraftPacketIds> supersedes;

    [[nodiscard]] constexpr bool isConfiguration() const noexcept {
        return lifecycle == PacketLifecycle::PreWorldHandshake || lifecycle == PacketLifecycle::SnapshotLatest
            || lifecycle == PacketLifecycle::SnapshotSequence;
    }

    [[nodiscard]] constexpr bool keepsSequence() const noexcept {
        return lifecycle == PacketLifecycle::SnapshotSequence;
    }

    [[nodiscard]] constexpr bool shouldReplayEverySnapshot() const noexcept {
        return replayEverySnapshot || keepsSequence();
    }
};

[[nodiscard]] PacketLifecycleSemantics describePacketLifecycle(MinecraftPacketIds packetId);

} // namespace playback::functions
