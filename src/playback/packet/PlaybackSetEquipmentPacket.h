#pragma once

#include "mc/deps/shared_types/legacy/EquipmentSlot.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/item/ItemStack.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

class Actor;
class Packet;

namespace playback::packet {

// Aggregates the equipment state of one actor before splitting it into the Bedrock packet shapes.
class PlaybackSetEquipmentPacket {
public:
    static constexpr std::size_t EquipmentSlotCount = 7;

    PlaybackSetEquipmentPacket(Actor const& actor, ActorRuntimeID runtimeId, int selectedSlot = 0);

    [[nodiscard]] std::vector<std::shared_ptr<Packet>> createPackets(
        PlaybackSetEquipmentPacket const* previous = nullptr
    ) const;

private:
    ActorRuntimeID                    mRuntimeId;
    int                               mSelectedSlot{};
    std::array<ItemStack, EquipmentSlotCount> mItems;

    [[nodiscard]] ItemStack const& item(SharedTypes::Legacy::EquipmentSlot slot) const;
};

} // namespace playback::packet
