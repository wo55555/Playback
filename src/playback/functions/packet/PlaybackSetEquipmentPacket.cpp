#include "PlaybackSetEquipmentPacket.h"

#include "mc/network/Packet.h"
#include "mc/network/packet/MobArmorEquipmentPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/item/NetworkItemStackDescriptor.h"

#include <utility>

namespace playback::functions {

PlaybackSetEquipmentPacket::PlaybackSetEquipmentPacket(Actor const& actor, ActorRuntimeID runtimeId, int selectedSlot)
    : mRuntimeId(runtimeId),
      mSelectedSlot(selectedSlot),
      mItems{
          actor.getCarriedItem(),
          actor.getOffhandSlot(),
          actor.getEquippedSlot(SharedTypes::Legacy::EquipmentSlot::Head),
          actor.getEquippedSlot(SharedTypes::Legacy::EquipmentSlot::Torso),
          actor.getEquippedSlot(SharedTypes::Legacy::EquipmentSlot::Legs),
          actor.getEquippedSlot(SharedTypes::Legacy::EquipmentSlot::Feet),
          actor.getEquippedSlot(SharedTypes::Legacy::EquipmentSlot::Body),
      } {}

ItemStack const& PlaybackSetEquipmentPacket::item(SharedTypes::Legacy::EquipmentSlot slot) const {
    return mItems[static_cast<std::size_t>(slot)];
}

std::vector<std::shared_ptr<Packet>> PlaybackSetEquipmentPacket::createPackets(
    PlaybackSetEquipmentPacket const* previous
) const {
    bool const runtimeChanged = previous == nullptr || mRuntimeId.rawID != previous->mRuntimeId.rawID;
    auto const changed = [this, previous, runtimeChanged](SharedTypes::Legacy::EquipmentSlot slot) {
        return runtimeChanged || previous == nullptr
            || !(static_cast<ItemStackBase const&>(item(slot))
                 == static_cast<ItemStackBase const&>(previous->item(slot)));
    };

    std::vector<std::shared_ptr<Packet>> packets;
    packets.reserve(3);

    bool const mainhandChanged = runtimeChanged || previous == nullptr || mSelectedSlot != previous->mSelectedSlot
        || changed(SharedTypes::Legacy::EquipmentSlot::Mainhand);
    if (mainhandChanged) {
        packets.emplace_back(std::make_shared<MobEquipmentPacket>(
            mRuntimeId,
            item(SharedTypes::Legacy::EquipmentSlot::Mainhand),
            mSelectedSlot,
            mSelectedSlot,
            ContainerID::Inventory
        ));
    }

    if (changed(SharedTypes::Legacy::EquipmentSlot::Offhand)) {
        packets.emplace_back(std::make_shared<MobEquipmentPacket>(
            mRuntimeId,
            item(SharedTypes::Legacy::EquipmentSlot::Offhand),
            0,
            0,
            ContainerID::Offhand
        ));
    }

    bool const armorChanged = changed(SharedTypes::Legacy::EquipmentSlot::Head)
        || changed(SharedTypes::Legacy::EquipmentSlot::Torso)
        || changed(SharedTypes::Legacy::EquipmentSlot::Legs)
        || changed(SharedTypes::Legacy::EquipmentSlot::Feet)
        || changed(SharedTypes::Legacy::EquipmentSlot::Body);
    if (armorChanged) {
        auto armor       = std::make_shared<MobArmorEquipmentPacket>();
        armor->mRuntimeId = mRuntimeId;
        armor->mHead      = NetworkItemStackDescriptor(item(SharedTypes::Legacy::EquipmentSlot::Head));
        armor->mTorso     = NetworkItemStackDescriptor(item(SharedTypes::Legacy::EquipmentSlot::Torso));
        armor->mLegs      = NetworkItemStackDescriptor(item(SharedTypes::Legacy::EquipmentSlot::Legs));
        armor->mFeet      = NetworkItemStackDescriptor(item(SharedTypes::Legacy::EquipmentSlot::Feet));
        armor->mBody      = NetworkItemStackDescriptor(item(SharedTypes::Legacy::EquipmentSlot::Body));
        packets.emplace_back(std::move(armor));
    }

    return packets;
}

} // namespace playback::functions
