#include "ReplaySession.h"

#include "playback/Playback.h"
#include "playback/action/Action.h"
#include "playback/editor/input/EditorInput.h"
#include "playback/integration/OptiPistonBridge.h"
#include "playback/io/ReplayResourcePacks.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/packet/PacketLifecycle.h"
#include "playback/record/Recorder.h"
#include "playback/runtime/ClientTickHooks.h"
#include "playback/runtime/GameAddress.h"
#include "playback/visuals/ReplayContainerKeepOpenHook.h"
#include "playback/visuals/ReplayEntityInterpolator.h"
#include "playback/visuals/ReplayFirstPersonHandHooks.h"
#include "playback/visuals/ReplayHudHooks.h"
#include "playback/visuals/ReplaySubjectHideHooks.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/client/model/GeometryGroup.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/particle/ParticleEngine.h"
#include "mc/client/particlesystem/particle/ParticleSystemEngine.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/deps/core/file/Path.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/strict/StrictEntityContext.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/deps/vanilla_components/OnGroundFlagComponent.h"
#include "mc/deps/vanilla_components/PlayerBobComponent.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/entity/components/DynamicRenderOffsetComponent.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/entity/components/ItemInUseTicksDuringMovementComponent.h"
#include "mc/entity/components/LocalPlayerDimensionWaitComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/entity/components/MobWalkAnimationComponent.h"
#include "mc/entity/components/MovementInterpolatorComponent.h"
#include "mc/entity/components/MovementWasCorrectedComponent.h"
#include "mc/entity/components/WalkDistComponent.h"
#include "mc/network/IPacketHandlerDispatcher.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/PacketSender.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/AddPaintingPacket.h"
#include "mc/network/packet/AddPlayerPacket.h"
#include "mc/network/packet/BlockActorDataPacket.h"
#include "mc/network/packet/BlockEventPacket.h"
#include "mc/network/packet/ChangeDimensionPacket.h"
#include "mc/network/packet/ClientboundMapItemDataPacket.h"
#include "mc/network/packet/ContainerClosePacket.h"
#include "mc/network/packet/ContainerOpenPacket.h"
#include "mc/network/packet/ContainerSetDataPacket.h"
#include "mc/network/packet/DimensionDataPacket.h"
#include "mc/network/packet/InventoryContentPacket.h"
#include "mc/network/packet/InventorySlotPacket.h"
#include "mc/network/packet/ItemData.h"
#include "mc/network/packet/ItemRegistryPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/MovePlayerPacket.h"
#include "mc/network/packet/PackInfoData.h"
#include "mc/network/packet/PacksInfoData.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerActionType.h"
#include "mc/network/packet/PlayerHotbarPacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketPayload.h"
#include "mc/network/packet/PlayerListPacketType.h"
#include "mc/network/packet/PlayerSkinPacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/RemoveObjectivePacket.h"
#include "mc/network/packet/ResourcePackStackPacket.h"
#include "mc/network/packet/ResourcePacksInfoPacket.h"
#include "mc/network/packet/SetDisplayObjectivePacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/StartGamePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/UpdateAttributesPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/network/packet/UpdateBlockSyncedPacket.h"
#include "mc/network/packet/UpdatePlayerGameTypePacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/network/packet/UpdateSubChunkNetworkBlockInfo.h"
#include "mc/network/packet/cerealize/core/SerializationMode.h"
#include "mc/resources/IResourcePackRepository.h"
#include "mc/resources/MinEngineVersion.h"
#include "mc/resources/PackInstance.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/server/NetworkChunkPublisher.h"
#include "mc/util/VarIntDataInput.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/LayeredAbilities.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerInventory.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/actor/player/SerializedSkinImpl.h"
#include "mc/world/actor/player/SerializedSkinRef.h"
#include "mc/world/attribute/Attribute.h"
#include "mc/world/attribute/AttributeData.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/attribute/AttributeInstanceRef.h"
#include "mc/world/attribute/AttributeModificationContext.h"
#include "mc/world/attribute/AttributeModifier.h"
#include "mc/world/attribute/BaseAttributeMap.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "mc/world/containers/managers/models/ContainerManagerModel.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/registry/ItemRegistryManager.h"
#include "mc/world/item/registry/ItemRegistryRef.h"
#include "mc/world/level/BlockPalette.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/DimensionManager.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/LevelSettings.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/TintMethod.h"
#include "mc/world/level/block/components/BlockComponentDescription.h"
#include "mc/world/level/block/components/BlockComponentStorageFinalizer.h"
#include "mc/world/level/block/components/BlockGeometryDescription.h"
#include "mc/world/level/block/components/BlockMaterialInstancesDescription.h"
#include "mc/world/level/block/components/BlockTypeComponentStorageFinalizer.h"
#include "mc/world/level/block/definition/BlockComponentGroupDescription.h"
#include "mc/world/level/block/definition/BlockDefinition.h"
#include "mc/world/level/block/definition/BlockDefinitionGroup.h"
#include "mc/world/level/block/definition/BlockStateDefinition.h"
#include "mc/world/level/block/definition/ServerBlockProperty.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/ChunkViewSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionArguments.h"
#include "mc/world/level/saveddata/maps/MapItemSavedData.h"
#include "mc/world/level/storage/ILevelListCache.h"
#include "mc/world/level/storage/LevelData.h"
#include "mc/world/phys/HitResult.h"
#include "mc/world/phys/HitResultType.h"



#include "snappy.h"
#include "uuid.h"
#include "zip.h"

#include <mc/world/actor/ActorType.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// Complete before ReplaySession's destructor so the saved hotbar can be freed.
struct FollowedHudMirror {
    bool                       active{};
    ::ActorUniqueID            subjectId{};
    int                        savedSelected{};
    int                        writtenSelected{-1};
    std::array<::ItemStack, 9> savedHotbar{};
    std::array<::ItemStack, 9> writtenHotbar{};
    std::array<::ItemStack, 4> savedArmor{};
    std::array<::ItemStack, 4> writtenArmor{};
    ::ItemStack                savedOffhand{};
    ::ItemStack                writtenOffhand{};
    // Recorded HUD packets arrive after the mirror has written, so they are kept here instead of applied.
    size_t diverted{};
    // The recorder's health, hunger and experience, put back on the observer when the mirror ends. AttributeData
    // cannot be copied (AttributeModifier's copy constructor is not exported), so only the values are kept.
    struct SavedAttribute {
        ::HashedString name;
        float          current{};
        float          min{};
        float          max{};
        float          defaultValue{};
        float          defaultMin{};
        float          defaultMax{};
    };
    std::vector<SavedAttribute> savedAttributes;
    // Set when the mirror starts or the followed player changes, so every slot is written once.
    bool forceWrite{};
};

namespace playback::replay {
using namespace playback::action;
using namespace playback::packet;
using namespace playback::record;

namespace {

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr std::string_view ReplayLevelIdPrefix        = "__playback_replay_world__";
constexpr auto             CenterChunkInjectionBudget = std::chrono::milliseconds(8);
constexpr auto             OuterChunkInjectionBudget  = std::chrono::milliseconds(4);
constexpr auto             SnapshotGamePacketBudget   = std::chrono::milliseconds(2);
// Every client tick a seek spills into is one more frame of visible catch-up, so land it in as few as possible.
constexpr auto SeekCatchUpBudget = std::chrono::milliseconds(2000);
// An interactive seek leaves the rest of each client tick to the editor, so it lands over a few frames instead of one.
constexpr auto SeekFrameBudget = std::chrono::milliseconds(100);
// Snapshot entities a seek waits on drain in one pass of up to this long instead of a few per client tick.
constexpr auto       SeekEntityPacketBudget = std::chrono::milliseconds(50);
constexpr std::array PlaybackSpeeds{0.05f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f};
// A seek keeps only the particle events from this many ticks before its landing, and ticks the level once per replay
// tick across them. Emitters such as lingering clouds are spawned by actor ticks and live about 27 ticks.
constexpr int SeekParticleWindowTicks = 40;

bool isSuccessfulSubChunkResult(SubChunkPacket::SubChunkRequestResult result) {
    return result == SubChunkPacket::SubChunkRequestResult::Success
        || result == SubChunkPacket::SubChunkRequestResult::SuccessAllAir;
}

std::optional<visuals::EntityRenderKey> replayEntityRenderKey(Level const& level, ActorUniqueID id) {
    auto const context = level.fetchStrictEntity(id, false);
    auto const entity  = *context.mEntity;
    if (entity.isNull()) return std::nullopt;
    return visuals::EntityRenderKey{context.mRegistryId, entity.mRawId};
}

visuals::EntityRenderPosition replayRenderPosition(Vec3 const& position) {
    return {position.x, position.y, position.z};
}

// Played ticks a missing entity subject keeps its choice before the view returns to the recorder.
constexpr int EntitySubjectGraceTicks = 20;

// First line of a name tag without its § formatting codes.
std::string plainNameTag(std::string const& tag) {
    std::string out;
    out.reserve(tag.size());
    for (size_t i = 0; i < tag.size(); ++i) {
        char const c = tag[i];
        if (c == '\n' || c == '\r') break;
        // § is C2 A7 in UTF-8; the code that follows it is one ASCII character.
        if (static_cast<unsigned char>(c) == 0xC2 && i + 1 < tag.size()
            && static_cast<unsigned char>(tag[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        out.push_back(c);
    }
    auto const first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

std::string entityTypeLabel(Actor const& actor) {
    std::string type = actor.getTypeName();
    if (type.starts_with("minecraft:")) type.erase(0, 10);
    return type;
}

// Feet to eyes. A mob's position is its feet, unlike a player's.
float entityEyeHeight(Actor const& actor) {
    float const eye = actor.getEyePos().y - actor.getPosition().y;
    if (std::isfinite(eye) && eye > 0.05f && eye < 64.0f) return eye;
    auto const& box    = actor.getAABB();
    float const height = box.max.y - box.min.y;
    return std::isfinite(height) && height > 0.0f ? height * 0.85f : 0.0f;
}

visuals::EntityRenderPose replayRenderPose(Actor const& actor) {
    auto const& context  = actor.getEntityContext();
    auto const  rotation = actor.getRotation();

    float headYaw = rotation.y;
    if (auto const headRotation = context.tryGetComponent<ActorHeadRotationComponent>()) {
        headYaw = headRotation->mYHeadRot;
    }

    float bodyYaw = rotation.y;
    if (auto const bodyRotation = context.tryGetComponent<MobBodyRotationComponent>()) {
        bodyYaw = bodyRotation->mYBodyRot;
    }

    return {replayRenderPosition(actor.getPosition()), rotation.x, rotation.y, headYaw, bodyYaw};
}

void cancelNativeMovementInterpolation(Actor& actor, Vec3 const& position, Vec2 const& rotation, float headYaw) {
    // Pin the pose directly so the vanilla movement system cannot re-apply the previous one.
    actor.mBuiltInComponents->mStateVectorComponent->mPos       = position;
    actor.mBuiltInComponents->mStateVectorComponent->mPosPrev   = position;
    actor.mBuiltInComponents->mStateVectorComponent->mPosDelta  = Vec3{};
    actor.mBuiltInComponents->mActorRotationComponent->mRot     = rotation;
    actor.mBuiltInComponents->mActorRotationComponent->mRotPrev = rotation;
    visuals::syncReplayEntityBoundingBox(actor, position);

    auto& context = actor.getEntityContext();
    context.removeComponent<MovementWasCorrectedComponent>();
    if (auto dynamicOffset = context.tryGetComponent<DynamicRenderOffsetComponent>()) {
        dynamicOffset->mCurrentOffset      = Vec3{};
        dynamicOffset->mNormalizedOffset   = Vec3{};
        dynamicOffset->mPreviousOffset     = Vec3{};
        dynamicOffset->mInterpolationSpeed = 0.0f;
        dynamicOffset->mCurrentPosDelta    = Vec3{};
    }

    auto const interpolator = context.tryGetComponent<MovementInterpolatorComponent>();
    if (!interpolator) return;

    *interpolator->mPos          = position;
    *interpolator->mRot          = rotation;
    interpolator->mHeadYaw       = headYaw;
    interpolator->mPositionSteps = 0;
    interpolator->mRotationSteps = 0;
    interpolator->mHeadYawSteps  = 0;
}

// Replay disables interpolation steps, so packet head/body yaw must be applied directly.
void pinReplayEntityHeadRotation(Actor& actor, float headYaw, float bodyYaw) {
    actor.setYHeadRotations(headYaw, headYaw);
    if (auto bodyRotation = actor.getEntityContext().tryGetComponent<MobBodyRotationComponent>()) {
        bodyRotation->mYBodyRot  = bodyYaw;
        bodyRotation->mYBodyRotO = bodyYaw;
    }
}

void updateAttribute(AttributeData const& data, AttributeInstanceRef instance, AttributeModificationContext& context) {
    static auto const update =
        runtime::gameFunction<void (*)(AttributeData const&, AttributeInstanceRef, AttributeModificationContext&)>(
            runtime::GameFunction::UpdateAttribute
        );
    update(data, instance, context);
}

// The attributes the HUD draws: hearts, absorption, hunger, saturation and the experience bar.
std::array<Attribute const*, 6> hudAttributes() {
    return {
        &SharedAttributes::HEALTH(),
        &SharedAttributes::ABSORPTION(),
        &Player::HUNGER(),
        &Player::SATURATION(),
        &Player::LEVEL(),
        &Player::EXPERIENCE(),
    };
}

bool isHudAttribute(HashedString const& name) {
    auto const attributes = hudAttributes();
    return std::any_of(attributes.begin(), attributes.end(), [&](Attribute const* attribute) {
        return *attribute->mName == name;
    });
}

// The HUD draws health, hunger, armor and experience from the observer's own attributes, so the recorded player's
// are copied onto it. Only those: movement speed would carry the recorder's sprint into the free camera's FOV.
// Health never drops below 1: a dead local player would open the death screen.
void mirrorRecordedAttributes(Player& observer, UpdateAttributesPacket& packet) {
    auto&                        attributes = *observer.getAttributes();
    AttributeModificationContext context{};
    context.mAttributeMap = &attributes;
    for (auto& data : *packet.mAttributeData) {
        if (!isHudAttribute(*data.mName)) continue;
        if (*data.mName == *SharedAttributes::HEALTH().mName && data.mCurrentValue < 1.0f) data.mCurrentValue = 1.0f;
        auto instance = attributes.getMutableInstance(Attribute::getByName(*data.mName));
        if (!instance.mPtr) continue;
        updateAttribute(data, instance, context);
    }
}

std::optional<FollowedHudMirror::SavedAttribute> readHudAttribute(Actor& actor, Attribute const& attribute) {
    auto const  instance = actor.getAttributes()->getMutableInstance(attribute);
    auto const* value    = instance.mPtr;
    if (!value) return std::nullopt;
    return FollowedHudMirror::SavedAttribute{
        *attribute.mName,
        value->mCurrentValue,
        value->mCurrentMinValue,
        value->mCurrentMaxValue,
        value->mDefaultValue,
        value->mDefaultMinValue,
        value->mDefaultMaxValue,
    };
}

void writeHudAttribute(Player& observer, FollowedHudMirror::SavedAttribute const& value) {
    auto const& attribute = Attribute::getByName(value.name);
    if (*attribute.mName != value.name) return;
    auto& attributes = *observer.getAttributes();
    auto  instance   = attributes.getMutableInstance(attribute);
    if (!instance.mPtr) return;
    static auto const construct = runtime::gameFunction<void* (*)(void*, AttributeInstance const&)>(
        runtime::GameFunction::AttributeDataConstructor
    );
    static auto const destroy = runtime::gameFunction<void (*)(void*)>(runtime::GameFunction::AttributeDataDestructor);
    alignas(AttributeData) std::byte storage[sizeof(AttributeData)];
    // Built from the live instance so the modifiers stay; only the values are replaced.
    auto& data            = *static_cast<AttributeData*>(construct(storage, *instance.mPtr));
    data.mCurrentValue    = value.current;
    data.mMinValue        = value.min;
    data.mMaxValue        = value.max;
    data.mDefaultValue    = value.defaultValue;
    data.mDefaultMinValue = value.defaultMin;
    data.mDefaultMaxValue = value.defaultMax;
    if (value.name == *SharedAttributes::HEALTH().mName && data.mCurrentValue < 1.0f) data.mCurrentValue = 1.0f;
    AttributeModificationContext context{};
    context.mAttributeMap = &attributes;
    updateAttribute(data, instance, context);
    destroy(&data);
}

// While another player's HUD is shown, the recorder's attribute updates are kept for the restore instead of applied.
void keepRecordedAttributes(FollowedHudMirror& mirror, UpdateAttributesPacket const& packet) {
    for (auto const& data : *packet.mAttributeData) {
        if (!isHudAttribute(*data.mName)) continue;
        auto const found = std::find_if(mirror.savedAttributes.begin(), mirror.savedAttributes.end(), [&](auto& kept) {
            return kept.name == *data.mName;
        });
        FollowedHudMirror::SavedAttribute value{
            *data.mName,
            data.mCurrentValue,
            data.mMinValue,
            data.mMaxValue,
            data.mDefaultValue,
            data.mDefaultMinValue,
            data.mDefaultMaxValue,
        };
        if (found != mirror.savedAttributes.end()) *found = std::move(value);
        else mirror.savedAttributes.push_back(std::move(value));
    }
}

// Bedrock's WalkDistComponent advances by exactly the horizontal distance moved (measured live, ratio 1.000).
constexpr float WalkDistancePerBlock = 1.0f;

// Items flagged to tick during movement (trident) are skipped by the normal use-duration system, and a replayed
// player has no move request to drive the movement one.
void tickReplayItemUse(Actor& actor) {
    auto& context = actor.getEntityContext();
    if (!context.hasComponent<ItemInUseTicksDuringMovementComponent>()) return;
    if (auto inUse = context.tryGetComponent<ItemInUseComponent>()) {
        int& remaining = inUse->mDuration;
        if (remaining > 1) --remaining;
    }
}

// Tick of the last real step written into each player's PlayerBobComponent. A tick with no movement decays it.
std::unordered_map<int64_t, int> gReplayHandBobTick;

// A remote player's inventory is never sent to clients; only the held item and its hotbar slot are (MobEquipment).
// The slots seen so far are kept per runtime id so a followed player's hotbar can be shown.
struct ReplayHotbar {
    std::array<ItemStack, 9> items{};
    int                      selected{-1};
};
std::unordered_map<uint64_t, ReplayHotbar> gReplayHotbars;

// PlayerTickBobSystem (1.26.20.04): the held map sways from this component. A remote player never runs that system.
void updateReplayHandBob(Actor& actor, float horizontalSpeed) {
    auto& context = actor.getEntityContext();
    auto  bob     = context.tryGetComponent<PlayerBobComponent>();
    if (!bob) {
        context.getRegistry().emplace<PlayerBobComponent>(context.mEntity);
        bob = context.tryGetComponent<PlayerBobComponent>();
        if (!bob) return;
    }
    float const target  = actor.getStatusFlag(ActorFlags::Swimming) ? 0.0f : std::min(0.1f, horizontalSpeed);
    bob->mBobPrev       = bob->mBob;
    bob->mBob          += (target - bob->mBob) * 0.08f;
}

void applyReplayEntityMovement(
    Actor&                           actor,
    Vec3 const&                      position,
    visuals::EntityRenderPose const& previousPose,
    Vec2 const&                      rotation,
    float                            headYaw,
    int                              tick
) {
    Vec3 const previousPosition{
        previousPose.position.x,
        previousPose.position.y,
        previousPose.position.z,
    };
    auto& state     = *actor.mBuiltInComponents->mStateVectorComponent;
    state.mPos      = position;
    state.mPosPrev  = previousPosition;
    state.mPosDelta = Vec3{
        position.x - previousPosition.x,
        position.y - previousPosition.y,
        position.z - previousPosition.z,
    };
    if (actor.isType(ActorType::Mob)) {
        auto& mob = static_cast<Mob&>(actor);
        if (auto walk = mob.mBuiltInMobComponents->mWalkAnimationComponent.get()) {
            // 26.51 dropped the HardcodedAnimationSystem.h helper, which only ever existed as a header inline; the
            // engine still folds this same horizontal-distance formula into its own tick, so compute it here.
            float const dx          = state.mPos->x - state.mPosPrev->x;
            float const dz          = state.mPos->z - state.mPosPrev->z;
            walk->mMovementThisTick = std::sqrt(dx * dx + dz * dz);
        }
    }
    // The native system needs a move request the replayed player never has; query.walk_distance drives the hand bob.
    if (actor.isPlayer()) {
        float const horizontal = std::hypot(state.mPosDelta->x, state.mPosDelta->z);
        if (auto distance = actor.getEntityContext().tryGetComponent<WalkDistComponent>()) {
            distance->mPrevious  = distance->mCurrent;
            distance->mCurrent  += horizontal * WalkDistancePerBlock;
        }
        updateReplayHandBob(actor, horizontal);
        gReplayHandBobTick[actor.getOrCreateUniqueID().rawID] = tick;
    }
    actor.mBuiltInComponents->mActorRotationComponent->mRot     = rotation;
    actor.mBuiltInComponents->mActorRotationComponent->mRotPrev = rotation;
    visuals::syncReplayEntityBoundingBox(actor, position);

    auto const interpolator = actor.getEntityContext().tryGetComponent<MovementInterpolatorComponent>();
    if (!interpolator) return;
    *interpolator->mPos          = position;
    *interpolator->mRot          = rotation;
    interpolator->mHeadYaw       = headYaw;
    interpolator->mPositionSteps = 0;
    interpolator->mRotationSteps = 0;
    interpolator->mHeadYawSteps  = 0;
}

std::string createReplayLevelId() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return std::string(ReplayLevelIdPrefix) + uuids::to_string(id);
}

bool isValidReplayLevelId(std::string_view levelId) {
    if (!levelId.starts_with(ReplayLevelIdPrefix)) return false;

    auto uuidText = levelId.substr(ReplayLevelIdPrefix.size());
    auto uuid     = uuids::uuid::from_string(uuidText);
    return uuid && uuids::to_string(*uuid) == uuidText;
}

std::optional<std::string> readArchiveEntry(zip_t* archive, std::string const& name) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat(archive, name.c_str(), 0, &stat) != 0) return std::nullopt;

    auto* file = zip_fopen(archive, name.c_str(), 0);
    if (!file) return std::nullopt;

    std::string data(static_cast<size_t>(stat.size), '\0');
    size_t      offset = 0;
    while (offset < data.size()) {
        auto read = zip_fread(file, data.data() + offset, data.size() - offset);
        if (read <= 0) {
            zip_fclose(file);
            return std::nullopt;
        }
        offset += static_cast<size_t>(read);
    }

    zip_fclose(file);
    return data;
}

bool appendChunkCache(std::string const& compressed, std::vector<std::string>& packets) {
    std::string data;
    if (!snappy::Uncompress(compressed.data(), compressed.size(), &data)) return false;

    ReadOnlyBinaryStream stream(data, false);
    while (stream.mReadPointer < data.size()) {
        if (data.size() - stream.mReadPointer < sizeof(uint32_t)) return false;

        uint32_t size = stream.getUnsignedInt().value();
        if (size > data.size() - stream.mReadPointer) return false;

        packets.emplace_back(data.data() + stream.mReadPointer, size);
        stream.mReadPointer += size;
    }
    return true;
}

struct InjectionReset {
    std::atomic<Packet const*>& injecting;
    ~InjectionReset() { injecting.store(nullptr, std::memory_order_release); }
};

float decodeRotationByte(uchar value) { return static_cast<float>(static_cast<schar>(value)) * (360.0f / 256.0f); }

// Like vanilla third person: back along the view ray from the eyes, stopping short of the first camera-blocking block.
keyframe::CameraRenderState thirdPersonFollowPose(Player& observer, keyframe::CameraRenderState state) {
    constexpr float Distance     = 4.0f;
    constexpr float WallMargin   = 0.2f;
    constexpr float DegToRadians = 0.017453292519943295f;
    float const     yaw          = state.yaw * DegToRadians;
    float const     pitch        = state.pitch * DegToRadians;
    // Opposite of the view direction {-sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)}.
    float const backX = std::sin(yaw) * std::cos(pitch);
    float const backY = std::sin(pitch);
    float const backZ = -std::cos(yaw) * std::cos(pitch);
    Vec3 const  eye{state.x, state.y, state.z};
    Vec3 const  target{eye.x + backX * Distance, eye.y + backY * Distance, eye.z + backZ * Distance};

    float      distance = Distance;
    auto const hit      = observer.getDimensionBlockSource().clip(
        eye,
        target,
        false,
        ShapeType::CollisionForCamera,
        static_cast<int>(Distance) + 1,
        false,
        false,
        nullptr,
        [](BlockSource const&, Block const&, bool) { return true; },
        false
    );
    if (hit.mType == HitResultType::Tile) {
        float const dx = hit.mPos.x - eye.x;
        float const dy = hit.mPos.y - eye.y;
        float const dz = hit.mPos.z - eye.z;
        distance       = std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz) - WallMargin, 0.0f, Distance);
    }
    state.x = eye.x + backX * distance;
    state.y = eye.y + backY * distance;
    state.z = eye.z + backZ * distance;
    return state;
}

void forceFirstPersonCamera() {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto& options = client->getOptions();
    if (options.getPlayerViewPerspective() != 0) options.setPlayerViewPerspective(0);
}

// A seek replays every particle event it crosses, so they would all pile up on the landing frame.
void clearReplayParticles() {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto* renderer = client->getLevelRenderer();
    if (!renderer) return;

    if (auto particles = renderer->mParticleEngine.get()) particles->clear();
    if (auto systems = renderer->mParticleSystemEngine.get()) systems->clear();
}

// The forced level ticks of a seek never tick the particle systems, which age by wall-clock time between their own
// ticks, so backdate that clock by one replay tick and tick them here.
void ageReplayParticles() {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto* renderer = client->getLevelRenderer();
    if (!renderer) return;
    auto systems = renderer->mParticleSystemEngine.get();
    if (!systems) return;

    systems->mLastTickTime      = std::chrono::steady_clock::now() - std::chrono::milliseconds(50);
    systems->mLastTickTimeValid = true;
    systems->tick(1.0f, false);
}

// Server teleport lands one eye height above the request; compensate so the feet agree.
constexpr float kServerEyeHeight = 1.62f;

// ContainerClose does not dismiss the player inventory. Pop it by name so a screen opened later is left alone.
void popReplayScreen() {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto const name = client->getTopScreenName();
    if (name != "inventory_screen" && name != "crafting_screen") return;
    client->getCurrentSceneStack()->schedulePopScreenWithExpectedNames(std::vector<std::string>{name});
    getLogger().info("screenPreview pop screen={}", name);
}

// The replay observer cannot use every container the recorder opened, so some are not previewed.
char const* containerPreviewSkip(Player& observer, ContainerOpenPacket& open, int& blockEntity) {
    using Type     = SharedTypes::Legacy::ContainerType;
    auto const raw = open.mEntityUniqueID.get().rawID;
    // The player inventory screen does not follow the recorded player yet, so it is not previewed.
    if (open.mType == Type::Inventory) return "skip:inventory";
    if (raw != -1 && raw != 0) {
        return observer.getLevel().fetchEntity(open.mEntityUniqueID.get(), false) ? nullptr : "skip:entityMissing";
    }
    auto const kind             = open.mType;
    bool const needsBlockEntity = kind == Type::Container || kind == Type::Furnace || kind == Type::BrewingStand
                               || kind == Type::Dispenser || kind == Type::Dropper || kind == Type::Hopper
                               || kind == Type::BlastFurnace || kind == Type::Smoker;
    if (!needsBlockEntity) return nullptr;
    blockEntity = observer.getDimensionBlockSource().getBlockEntity(open.mPos.get()) ? 1 : 0;
    return blockEntity ? nullptr : "skip:noBlockEntity";
}

bool sameHudStack(ItemStack const& left, ItemStack const& right) {
    if (left.isNull() || right.isNull()) return left.isNull() && right.isNull();
    return left.getId() == right.getId() && left.hasSameAuxValue(right) && left.mCount == right.mCount
        && left.hasSameUserData(right);
}

ItemStack itemFromDescriptor(NetworkItemStackDescriptor const& descriptor, BlockPalette const& palette) {
    static auto const getItemStack =
        runtime::gameMember<ItemStack (NetworkItemStackDescriptor::*)(BlockPalette const&) const>(
            runtime::GameFunction::NetworkItemStackDescriptorGetItemStack
        );
    return (descriptor.*getItemStack)(palette);
}

constexpr std::array<SharedTypes::Legacy::ArmorSlot, 4> HudArmorSlots{
    SharedTypes::Legacy::ArmorSlot::Head,
    SharedTypes::Legacy::ArmorSlot::Torso,
    SharedTypes::Legacy::ArmorSlot::Legs,
    SharedTypes::Legacy::ArmorSlot::Feet,
};

// While another player's HUD is shown, the recorded local player's inventory packets describe the observer. Keeping
// them in the mirror's saved slots lets the restore put the recorder's items back, instead of writing them now.
bool divertRecordedHudPacket(Packet& packet, FollowedHudMirror& mirror) {
    auto const level  = ll::service::getMultiPlayerLevel();
    auto const toItem = [&](NetworkItemStackDescriptor const& descriptor) {
        return itemFromDescriptor(descriptor, level->getBlockPalette());
    };

    switch (packet.getId()) {
    case MinecraftPacketIds::InventorySlot: {
        if (!level) return true;
        auto&     slotPacket = static_cast<InventorySlotPacket&>(packet);
        int const slot       = static_cast<int>(slotPacket.mSlot);
        if (slotPacket.mInventoryId == ContainerID::Inventory && slot >= 0 && slot < 9) {
            mirror.savedHotbar[static_cast<size_t>(slot)] = toItem(*slotPacket.mItem);
        } else if (slotPacket.mInventoryId == ContainerID::Armor && slot >= 0 && slot < 4) {
            mirror.savedArmor[static_cast<size_t>(slot)] = toItem(*slotPacket.mItem);
        } else if (slotPacket.mInventoryId == ContainerID::Offhand && slot == 0) {
            mirror.savedOffhand = toItem(*slotPacket.mItem);
        }
        ++mirror.diverted;
        return true;
    }
    case MinecraftPacketIds::InventoryContent: {
        if (!level) return true;
        auto& content = static_cast<InventoryContentPacket&>(packet);
        auto& slots   = *content.mSlots;
        if (content.mInventoryId == ContainerID::Inventory) {
            int const count = std::min(static_cast<int>(slots.size()), 9);
            for (int slot = 0; slot < count; ++slot) {
                mirror.savedHotbar[static_cast<size_t>(slot)] = toItem(slots[static_cast<size_t>(slot)]);
            }
        } else if (content.mInventoryId == ContainerID::Armor) {
            int const count = std::min(static_cast<int>(slots.size()), 4);
            for (int slot = 0; slot < count; ++slot) {
                mirror.savedArmor[static_cast<size_t>(slot)] = toItem(slots[static_cast<size_t>(slot)]);
            }
        } else if (content.mInventoryId == ContainerID::Offhand && !slots.empty()) {
            mirror.savedOffhand = toItem(slots.front());
        }
        ++mirror.diverted;
        return true;
    }
    case MinecraftPacketIds::PlayerHotbar: {
        auto const& hotbar = static_cast<PlayerHotbarPacket const&>(packet);
        if (hotbar.mContainerId == ContainerID::Inventory && hotbar.mShouldSelectSlot) {
            mirror.savedSelected = std::clamp(static_cast<int>(hotbar.mSelectedSlot), 0, 8);
        }
        ++mirror.diverted;
        return true;
    }
    case MinecraftPacketIds::PlayerEquipment: {
        auto& equipment = static_cast<MobEquipmentPacket&>(packet);
        if (equipment.mRuntimeId->rawID != record::RecordedPlayerRuntimeIdValue) return false;
        if (equipment.mContainerId != ContainerID::Inventory || !level) return true;
        int const slot                                = std::clamp(static_cast<int>(equipment.mSlot), 0, 8);
        mirror.savedHotbar[static_cast<size_t>(slot)] = toItem(*equipment.mItem);
        mirror.savedSelected                          = std::clamp(static_cast<int>(equipment.mSelectedSlot), 0, 8);
        ++mirror.diverted;
        return true;
    }
    default:
        return false;
    }
}

} // namespace

ReplaySession::ReplaySession()  = default;
ReplaySession::~ReplaySession() = default;

bool ReplaySession::start(std::filesystem::path filePath) {
    if (mActive || mCleanupState != CleanupState::None || !mReplayLevelId.empty()) {
        getLogger().error("Unable to start replay while another replay world is active or being removed");
        return false;
    }
    auto client = ll::service::getClientInstance();
    if (!client || ll::service::getMultiPlayerLevel() || client->hasLevel() || client->isWorldActive()
        || !client->isLeaveGameDone()) {
        getLogger().error("Replay can only be started from the main menu");
        return false;
    }
    auto& game = client->getMinecraftGame_DEPRECATED();
    if (game.isInServer() || game.getServerInstance()) {
        getLogger().error("Replay cannot start until the current world server has completely stopped");
        return false;
    }

    auto screenModel = mScreenModel.lock();
    if (!screenModel) {
        getLogger().error("Unable to start replay because the main menu is not ready");
        return false;
    }

    forceFirstPersonCamera();
    // Installed once per replay (inert until the subject is hidden) so toggling follow never hooks anything.
    visuals::hookReplaySubjectHide(true, visuals::SubjectHideReason::Session);
    visuals::hookReplayFirstPersonHands(true, visuals::SubjectHideReason::Session);
    visuals::installReplayHudHooks();

    try {
        if (!init(std::move(filePath))) {
            stop();
            return false;
        }
        if (mSnapshotContexts.empty()) throw std::runtime_error("Replay contains no snapshot contexts");
        auto const configurationPackets = mReaders.front()->readConfigurationPackets();
        if (!prepareReplayResourcePacks(configurationPackets)) {
            throw std::runtime_error("Unable to prepare the recorded resource-pack configuration");
        }
        prepareRecordedBlockRegistry(configurationPackets);
        prepareRecordedItemRegistry(configurationPackets);
        auto const& context = mSnapshotContexts.front();

        LevelSettings settings;
        settings.mGameType                  = GameType::Spectator;
        settings.mForceGameType             = true;
        settings.mGenerator                 = GeneratorType::Void;
        settings.mImmutableWorld            = true;
        settings.mMultiplayerGameIntent     = false;
        settings.mLANBroadcastIntent        = false;
        settings.mDisablePlayerInteractions = true;
        settings.mDefaultSpawn              = BlockPos(Vec3{context.x, context.y, context.z});
        if (mReplayStartGame) settings.mSeed = mReplayStartGame->mSettings->mSeed;

        mReplayLevelId            = createReplayLevelId();
        auto dimensionProfile     = std::make_shared<ReplayDimensionProfile>();
        dimensionProfile->levelId = mReplayLevelId;
        for (auto const& snapshot : mSnapshotContexts) {
            auto const minimum = snapshot.dimensionMinHeight;
            auto const maximum = snapshot.dimensionMaxHeight;
            auto const height  = static_cast<int64_t>(maximum) - static_cast<int64_t>(minimum);
            if (minimum < std::numeric_limits<short>::min() || maximum > std::numeric_limits<short>::max()
                || minimum % 16 != 0 || maximum <= minimum || height % 16 != 0) {
                throw std::runtime_error(
                    std::format(
                        "Replay dimension {} has invalid recorded height range [{}, {})",
                        snapshot.dimensionId,
                        minimum,
                        maximum
                    )
                );
            }

            if (!snapshot.dimensionName.empty()) {
                dimensionProfile->names.insert_or_assign(snapshot.dimensionId, snapshot.dimensionName);
            }

            RecordedDimensionHeightRange const range{minimum, maximum};
            auto const [it, inserted] = dimensionProfile->heightRanges.emplace(snapshot.dimensionId, range);
            if (!inserted && it->second != range) {
                throw std::runtime_error(
                    std::format(
                        "Replay dimension {} has conflicting recorded height ranges [{}, {}) and [{}, {})",
                        snapshot.dimensionId,
                        it->second.minimum,
                        it->second.maximum,
                        minimum,
                        maximum
                    )
                );
            }
        }
        mReplayDimensionProfile.store(std::move(dimensionProfile), std::memory_order_release);

        mCleanupState = CleanupState::None;
        mActive       = true;
        integration::pushReplayClock(mCurrentTick, true);
        screenModel->startLocalServerAsync(mReplayLevelId, "Playback Replay", settings);
        getLogger().debug("Starting replay from {} in {}", mReplayFilePath, mReplayLevelId);
        return true;
    } catch (std::exception const& e) {
        getLogger().error("Unable to start replay: {}", e.what());
        stop();
        return false;
    }
}

void ReplaySession::clearReplayData() {
    restoreFollowedPlayerHud();
    restoreObserverSkin();
    {
        std::lock_guard lock(mReplayItemRegistryMutex);
        mReplayItemRegistryPayload.clear();
    }
    mReplayDimensionProfile.store({}, std::memory_order_release);
    mStopRequested.store(false, std::memory_order_release);
    mRequestedSeekTick.store(-1, std::memory_order_release);
    integration::clearReplayClock();
    mActive       = false;
    mIsPaused     = false;
    mWorldReady   = false;
    mReplayFailed = false;
    mInjectingPacket.store(nullptr, std::memory_order_release);
    mScreenPreview.reset();
    mRecordedScreens.clear();
    mTriedScreenOpenTick = -1;
    visuals::setReplayContainerKeepOpen(false);
    visuals::setReplayHudGameType(-1);
    mLevelSimFrozen       = false;
    mSeekParticlesCleared = false;
    mSeekParticlesStale   = false;
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mReplayDimension.store(nullptr, std::memory_order_release);
    mInitialSnapshotApplied = false;
    mChunkInjectionPending  = false;
    clearSeekChunkDeferral();
    mChunkCenterLogKey.clear();
    mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    mIsProcessingSnapshot    = false;
    mCurrentTick             = 0;
    mReaderIndex             = 0;
    mSeekTargetTick          = -1;
    mExportSeekRequested     = false;
    mSnapMovementDuringSeek  = false;
    mExportTimelinePhase     = ReplayExportTimelinePhase::Inactive;
    mExportTargetTick        = -1;
    mExportCameraViewpoint.reset();
    mPlaybackSpeed           = 1.0f;
    mPlaybackTickAccumulator = 0.0f;
    mObserverPreviewInRange  = false;
    mPausedPreviewPose.reset();
    clearPreviewShot();
    setFollowRecordedPlayer(false);
    mFollowExitParkPending    = false;
    mTeleportToSubjectPending = false;
    mPendingObserverPark.reset();
    mObserverServerPositionDirty = false;
    mLastObserverServerSyncChunk.reset();
    mObserverServerSyncEpoch.fetch_add(1, std::memory_order_acq_rel);
    mReplayTime.reset();
    mPendingReplayDimension.reset();
    mPendingSnapshotApply.reset();
    if (mDimensionTransitionRequest) {
        mDimensionTransitionRequest->status.store(DimensionTransitionStatus::Cancelled, std::memory_order_release);
        mDimensionTransitionRequest.reset();
    }
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;

    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionStartedAt      = {};

    mCompletedDimensionGeneration  = 0;
    mDimensionTransitionGeneration = 0;

    mInjectedLevelChunks     = 0;
    mInjectedSubChunkPackets = 0;
    mInjectedSubChunkEntries = 0;
    mReusedSnapshotColumns   = 0;
    mDirectLevelChunks       = 0;
    mDirectSubChunkPackets   = 0;
    mDirectSubChunkEntries   = 0;
    mReplayPlayer            = nullptr;
    mNetworkHandler          = nullptr;
    mReplayFilePath.clear();
    mMeta = PlaybackMeta{};
    mReaders.clear();
    mSnapshotContexts.clear();
    mDimensionTransitionTicks.clear();
    mChunkPackets.clear();
    mInlineLevelChunkPacketIndices.clear();
    mInlineSubChunkPacketIndices.clear();
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
        mRetainedReplayChunks.clear();
    }
    mPendingLevelChunkIndices.clear();
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mPendingSnapshotLocalPlayer.reset();
    mPendingSnapshotGamePackets.clear();
    mAppliedConfigurationPackets.clear();
    mRecordedEntityIds.clear();
    mRecordedLocalPlayerId.reset();
    gReplayHandBobTick.clear();
    gReplayHotbars.clear();
    clearReplayPlayers(true);
    visuals::setReplaySubject(std::nullopt);
    mEntityRenderKeys.clear();
    visuals::clearReplayEntityPoses();
    mReplayObjectiveNames.clear();
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mApplyingChunkSnapshot      = false;
    mChunkInjectionPlanPrepared = false;
    mCenterChunksReady          = false;
    mChunkInjectionStartedAt    = {};
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
}

void ReplaySession::finishWorldCleanup() {
    releaseReplayResourcePacks();
    clearReplayData();
    mReplayWorldJoined = false;
    mCleanupState      = CleanupState::None;
    mCleanupWaitTicks  = 0;
    mReplayLevelId.clear();
    mOrphanReplayWorldsScanned = false;
}

void ReplaySession::stop() {
    if (mReplayLevelId.empty()) {
        finishWorldCleanup();
        return;
    }
    if (mCleanupState != CleanupState::None) return;

    if (auto level = ll::service::getMultiPlayerLevel(); level && !isReplayLevel(level.value())) {
        getLogger().error("Cancelling replay without leaving the non-replay world {}", level->getLevelId());
        mCleanupState     = CleanupState::ReadyToDelete;
        mCleanupWaitTicks = 0;
        clearReplayData();
        mReplayWorldJoined = false;
        return;
    }

    mCleanupState     = CleanupState::WaitingForExit;
    mCleanupWaitTicks = 0;
    clearReplayData();

    auto client = ll::service::getClientInstance();
    if (!client) {
        getLogger().error("Unable to leave replay world {} because the client is unavailable", mReplayLevelId);
        return;
    }

    getLogger().debug("Leaving replay world {}", mReplayLevelId);
    client->requestLeaveGameAsync();
}

bool ReplaySession::setPaused(bool paused) {
    if (!mActive) return false;
    if (mIsPaused == paused) return true;

    bool const wasPreviewing = keyframe::wasPreviewCameraApplied();
    // Hold the fraction the preview stopped at, so resuming continues from the same pose instead of the tick boundary.
    if (paused) mFrozenPreviewPartial.store(previewPartialTick(), std::memory_order_release);
    mIsPaused = paused;
    // Pausing parks the observer and resuming hands it to the timeline or the player; neither keeps the old pose.
    mObserverPreviewInRange = false;
    if (!paused) {
        mPausedPreviewPose.reset();
        mObserverServerSyncEpoch.fetch_add(1, std::memory_order_acq_rel);
        mObserverServerPositionDirty = false;
        mLastObserverServerSyncChunk.reset();
        resumePreviewClockFromFrozenPartial();
    }
    getLogger().debug("Replay {} at tick {}", paused ? "paused" : "playing", mCurrentTick);
    if (paused && mExportTimelinePhase == ReplayExportTimelinePhase::Inactive && wasPreviewing) {
        parkReplayCameraAtPreview();
    } else if (mReplayPlayer && mReplayWorldJoined) {
        auto const position = mReplayPlayer->getPosition();
        auto const rotation = mReplayPlayer->getRotation();
        if (paused) {
            cancelNativeMovementInterpolation(*mReplayPlayer, position, rotation, rotation.y);
            if (mObserverServerPositionDirty) syncObserverServerPosition(position, rotation);
        } else if (mExportTimelinePhase == ReplayExportTimelinePhase::Inactive) {
            // The epoch bump dropped any sync still queued (such as the one an export end sends); resend it.
            syncObserverServerPosition(position, rotation);
        }
    }
    return true;
}

void ReplaySession::parkReplayCameraAtPreview() {
    keyframe::setPreviewCameraApplied(false);
    if (!mReplayPlayer || !mReplayWorldJoined || !mNetworkHandler) return;
    if (mPendingReplayDimension) return;

    auto const sampleTime = getCameraRenderSampleTime();
    if (!sampleTime) return;
    auto const sample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Preview, *sampleTime);
    if (!sample) return;

    auto const lastPose = keyframe::takeLastPreviewPose();
    parkObserverAtPose(lastPose.value_or(sample->state));
}

void ReplaySession::parkObserverAtPose(keyframe::CameraRenderState const& pose) {
    Vec3 const feetPosition{pose.x, pose.y, pose.z};
    Vec2 const rotation{pose.pitch, pose.yaw};
    forceFirstPersonCamera();
    teleportReplayPlayer(feetPosition, rotation);
    cancelNativeMovementInterpolation(*mReplayPlayer, feetPosition, rotation, rotation.y);
    syncObserverServerPosition(feetPosition, rotation);
}

// On a paused seek, park the observer at the pose the preview timeline samples here.
void ReplaySession::updatePausedObserverPreview() {
    if (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive || !mNetworkHandler) return;
    if (!isReplayWorldReady() || mPendingSnapshotApply || isSeekPending()) return;
    if (!keyframe::hasCameraTimeline(keyframe::CameraTimelineSource::Preview)) {
        mPausedPreviewPose.reset();
        return;
    }
    // Player input owns the camera; only a new seek or a changed sample takes it back.
    if (editor::input::isGameInputCaptured()) return;

    auto const time = getCameraRenderSampleTime();
    if (!time) return;
    auto const sample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Preview, *time);
    if (!sample) {
        mPausedPreviewPose.reset();
        return;
    }

    auto const seekCount = seekFinishCount();
    if (mPausedPreviewPose == sample->state && mPausedPreviewSeekCount == seekCount) return;
    mPausedPreviewPose      = sample->state;
    mPausedPreviewSeekCount = seekCount;
    parkObserverAtPose(sample->state);
}

void ReplaySession::applyFollowView() {
    bool const active = isFollowViewActive();
    bool const third  = active && isFollowViewThirdPerson();
    if (active != mFollowViewApplied) {
        mFollowViewApplied = active;
        // The camera sits inside the followed player, so its own model would fill the view. The hide and hand
        // hooks stay installed for the whole replay; (un)hooking them here froze the game for seconds.
        visuals::setReplaySubjectHidden(visuals::SubjectHideReason::Follow, active);
        mLastObserverServerSyncChunk.reset();
        mPausedPreviewPose.reset();
        // The server copy only moved on chunk changes while following; park it so its next correction does not
        // snap back.
        mFollowExitParkPending = !active;
    }
    if (third != mFollowViewThirdApplied) {
        mFollowViewThirdApplied = third;
        visuals::setReplaySubjectThirdPerson(third);
        mFollowHeldPose.reset();
    }
}

void ReplaySession::setFollowRecordedPlayer(bool follow) {
    if (mFollowRecordedPlayer.exchange(follow, std::memory_order_acq_rel) == follow) return;
    if (!follow) mFollowThirdPerson.store(false, std::memory_order_release);
    applyFollowView();
    getLogger().debug("Observer {} the recorded player", follow ? "follows" : "stopped following");
}

void ReplaySession::setFollowThirdPerson(bool thirdPerson) {
    if (thirdPerson && !isFollowingRecordedPlayer()) thirdPerson = false;
    if (mFollowThirdPerson.exchange(thirdPerson, std::memory_order_acq_rel) == thirdPerson) return;
    applyFollowView();
    getLogger().debug("Follow view switched to {} person", thirdPerson ? "third" : "first");
}

void ReplaySession::setShotSubject(std::optional<std::string> subject) {
    {
        std::lock_guard lock(mReplayPlayersMutex);
        if (mShotSubject == subject) return;
        mShotSubject = std::move(subject);
    }
    mReplaySubjectEntityMissingTicks    = 0;
    mReplaySubjectEntityMissingLastTick = -1;
    rebindReplaySubject();
}

void ReplaySession::clearPreviewShot() {
    mPreviewShotMode.store(PreviewShotMode::None, std::memory_order_release);
    mShotThirdPerson.store(false, std::memory_order_release);
    setShotSubject(std::nullopt);
    applyFollowView();
}

// The highest camera track under the playhead owns the view. A keyframe track suspends the
// user's follow; a person track follows its subject; with no track the user's follow comes back.
void ReplaySession::updatePreviewShot() {
    if (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive) return;
    using state::editing::model::CameraTrackKind;
    auto                       mode = PreviewShotMode::None;
    std::optional<std::string> subject;
    bool                       third = false;
    // Player input owns the camera while captured, as it does over a paused keyframe track.
    if (!editor::input::isGameInputCaptured()) {
        if (auto const timeline = keyframe::currentCameraTimeline(keyframe::CameraTimelineSource::Preview)) {
            auto const time = getCameraRenderSampleTime();
            auto const shot = timeline->shotAt(time ? time->floorTick() : getCurrentTick());
            if (shot && shot.kind() == CameraTrackKind::Keyframe) {
                mode = PreviewShotMode::Keyframe;
            } else if (shot) {
                mode    = PreviewShotMode::Person;
                subject = shot.subject;
                third   = shot.kind() == CameraTrackKind::ThirdPerson;
            }
        }
    }
    mShotThirdPerson.store(third, std::memory_order_release);
    if (mPreviewShotMode.exchange(mode, std::memory_order_acq_rel) != mode) {
        getLogger().debug(
            "preview shot mode={} subject={} third={} tick={}",
            static_cast<int>(mode),
            subject.value_or(""),
            third,
            mCurrentTick
        );
    }
    setShotSubject(std::move(subject));
    applyFollowView();
}

keyframe::CameraRenderState ReplaySession::thirdPersonView(keyframe::CameraRenderState const& eye) const {
    return mReplayPlayer ? thirdPersonFollowPose(*mReplayPlayer, eye) : eye;
}

void ReplaySession::parkObserverAfterFollow() {
    if (!mFollowExitParkPending) return;
    // An export drives the observer and syncs it when it ends; a dimension change teleports it on both sides.
    if (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive || mPendingReplayDimension) {
        mFollowExitParkPending = false;
        return;
    }
    if (!mReplayPlayer || !mReplayWorldJoined || !mNetworkHandler) return;
    mFollowExitParkPending = false;
    if (auto const pose = mFollowHeldPose ? mFollowHeldPose : sampleSubjectFollowState()) {
        parkObserverAtPose(*pose);
        return;
    }
    // No pose to park at (a seek is streaming in): pin the server to where following left the client.
    auto const position = mReplayPlayer->getPosition();
    auto const rotation = mReplayPlayer->getRotation();
    syncObserverServerPosition(position, rotation);
}

// The preview moved the server copy only on chunk changes; once it stops driving the observer, pin the server.
void ReplaySession::releasePreviewObserver() {
    mLastObserverServerSyncChunk.reset();
    if (!std::exchange(mObserverPreviewInRange, false)) return;
    if (!mReplayPlayer || !mReplayWorldJoined || !mNetworkHandler) return;
    teleportReplayPlayer(mLastObserverPreviewFeet, mLastObserverPreviewRotation);
    cancelNativeMovementInterpolation(
        *mReplayPlayer,
        mLastObserverPreviewFeet,
        mLastObserverPreviewRotation,
        mLastObserverPreviewRotation.y
    );
    syncObserverServerPosition(mLastObserverPreviewFeet, mLastObserverPreviewRotation);
}

namespace {
// Servers such as Hive append "\nLevel N" to name tags. Menus and logs need a single line.
std::string firstNameLine(std::string name) {
    if (auto const cut = name.find_first_of("\r\n"); cut != std::string::npos) name.resize(cut);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    return name;
}
} // namespace

void ReplaySession::rememberReplayPlayer(std::string uuid, std::string name, ActorUniqueID id, bool recorder) {
    name = firstNameLine(std::move(name));
    std::lock_guard lock(mReplayPlayersMutex);
    auto const      found = mReplayPlayers.find(uuid);
    if (found == mReplayPlayers.end()) {
        mReplayPlayers.emplace(std::move(uuid), ReplayPlayerRecord{id, std::move(name), recorder, true});
        return;
    }
    found->second.id       = id;
    found->second.recorder = recorder;
    found->second.present  = true;
    if (!name.empty()) found->second.name = std::move(name);
}

void ReplaySession::noteReplayIdentity(std::string uuid, std::string name, ActorUniqueID id, bool recorder) {
    name = firstNameLine(std::move(name));
    std::lock_guard lock(mReplayPlayersMutex);
    auto const      found = mReplayPlayers.find(uuid);
    if (found == mReplayPlayers.end()) {
        mReplayPlayers.emplace(std::move(uuid), ReplayPlayerRecord{id, std::move(name), recorder, false});
        return;
    }
    found->second.id       = id;
    found->second.recorder = recorder;
    if (!name.empty()) found->second.name = std::move(name);
}

void ReplaySession::forgetReplayIdentity(std::string const& uuid) {
    std::lock_guard lock(mReplayPlayersMutex);
    mReplayPlayers.erase(uuid);
}

void ReplaySession::markReplayPlayerAbsent(ActorUniqueID id) {
    std::lock_guard lock(mReplayPlayersMutex);
    for (auto& [uuid, record] : mReplayPlayers) {
        (void)uuid;
        if (record.id == id) record.present = false;
    }
}

void ReplaySession::clearReplayPlayers(bool clearChoice) {
    restoreObserverSkin();
    std::lock_guard lock(mReplayPlayersMutex);
    if (!clearChoice) {
        for (auto& [uuid, record] : mReplayPlayers) {
            (void)uuid;
            record.present = false;
        }
        return;
    }
    mReplayPlayers.clear();
    mReplaySubjectChoice.clear();
    mShotSubject.reset();
    mReplaySubjectGapChoice.clear();
    mReplaySubjectEntityMissingTicks    = 0;
    mReplaySubjectEntityMissingLastTick = -1;
    {
        std::lock_guard entitiesLock(mReplayEntitiesMutex);
        mReplayEntities.clear();
    }
    mObserverOriginalSkin.reset();
    mObserverSkinApplied = false;
}

void ReplaySession::refreshReplayPlayerRoster() {
    if (!mReplayPlayer || mPendingReplayDimension || mPendingSnapshotApply || mChunkInjectionPending) return;

    auto&      level     = mReplayPlayer->getLevel();
    auto const dimension = mReplayPlayer->getDimensionId();
    auto const actors    = level.getRuntimeActorList();
    struct LivePlayer {
        ActorUniqueID id;
        std::string   name;
        bool          recorder{};
    };
    std::unordered_map<std::string, LivePlayer> live;
    live.reserve(actors.size());
    auto const collectPlayer = [&](Player const& player) {
        if (&player == mReplayPlayer || player.getDimensionId() != dimension) return;
        std::string uuid = player.getUuid().asString();
        if (uuid.empty()) return;
        auto const uniqueId = player.getOrCreateUniqueID();
        bool const recorder = mRecordedLocalPlayerId && uniqueId == *mRecordedLocalPlayerId;
        live.insert_or_assign(std::move(uuid), LivePlayer{uniqueId, firstNameLine(player.getNameTag()), recorder});
    };
    for (auto const* actor : actors) {
        if (!actor || !actor->isPlayer()) continue;
        collectPlayer(static_cast<Player const&>(*actor));
    }
    level.forEachPlayer([&](Player& player) {
        collectPlayer(player);
        return true;
    });

    std::lock_guard                 lock(mReplayPlayersMutex);
    bool                            changed = false;
    std::unordered_set<std::string> spawned;
    spawned.reserve(live.size());
    for (auto& [uuid, info] : live) {
        spawned.insert(uuid);
        auto const found = mReplayPlayers.find(uuid);
        if (found == mReplayPlayers.end()) {
            auto name = info.name.empty() ? uuid : std::move(info.name);
            mReplayPlayers.emplace(uuid, ReplayPlayerRecord{info.id, std::move(name), info.recorder, true});
            changed = true;
            continue;
        }
        auto& record = found->second;
        if (record.id != info.id || record.recorder != info.recorder || !record.present) changed = true;
        record.id       = info.id;
        record.recorder = info.recorder;
        record.present  = true;
        // The player list name is the identity; the name tag only fills a missing one.
        bool const unnamed = record.name.empty() || record.name == uuid;
        if (unnamed && !info.name.empty() && record.name != info.name) {
            record.name = std::move(info.name);
            changed     = true;
        }
    }
    for (auto& [uuid, record] : mReplayPlayers) {
        if (spawned.contains(uuid) || !record.present) continue;
        record.present = false;
        changed        = true;
    }
    if (!changed) return;
    int present = 0;
    for (auto const& [uuid, record] : mReplayPlayers) {
        (void)uuid;
        if (record.present) ++present;
    }
    getLogger().debug(
        "replay player list refreshed present={} identity={} tick={}",
        present,
        mReplayPlayers.size(),
        mCurrentTick
    );
}

std::string ReplaySession::replaySubjectChoice() const {
    std::lock_guard lock(mReplayPlayersMutex);
    return mShotSubject ? *mShotSubject : mReplaySubjectChoice;
}

std::string ReplaySession::userReplaySubjectChoice() const {
    std::lock_guard lock(mReplayPlayersMutex);
    return mReplaySubjectChoice;
}

std::vector<ReplaySession::ReplayPlayerInfo> ReplaySession::replayPlayers() const {
    std::vector<ReplayPlayerInfo> recorder;
    std::vector<ReplayPlayerInfo> others;
    {
        std::lock_guard lock(mReplayPlayersMutex);
        recorder.reserve(mReplayPlayers.size());
        for (auto const& [uuid, record] : mReplayPlayers) {
            ReplayPlayerInfo info{uuid, record.name, record.recorder, record.present};
            if (record.recorder) recorder.push_back(std::move(info));
            else others.push_back(std::move(info));
        }
    }
    auto const byName = [](ReplayPlayerInfo const& a, ReplayPlayerInfo const& b) { return a.name < b.name; };
    std::sort(recorder.begin(), recorder.end(), byName);
    std::sort(others.begin(), others.end(), byName);
    recorder.insert(recorder.end(), others.begin(), others.end());
    return recorder;
}

void ReplaySession::setReplaySubjectChoice(std::string uuid) {
    bool changed = false;
    {
        std::lock_guard lock(mReplayPlayersMutex);
        changed              = mReplaySubjectChoice != uuid;
        mReplaySubjectChoice = std::move(uuid);
    }
    if (changed) {
        mReplaySubjectEntityMissingTicks    = 0;
        mReplaySubjectEntityMissingLastTick = -1;
        getLogger().debug("replay subject choice={}", replaySubjectChoice());
    }
    rebindReplaySubject();
}

std::optional<int64_t> ReplaySession::parseEntityChoice(std::string_view choice) {
    if (!choice.starts_with(EntityChoicePrefix)) return std::nullopt;
    choice.remove_prefix(EntityChoicePrefix.size());
    int64_t value{};
    auto const [end, error] = std::from_chars(choice.data(), choice.data() + choice.size(), value);
    if (error != std::errc{} || end != choice.data() + choice.size()) return std::nullopt;
    return value;
}

std::optional<ActorUniqueID> ReplaySession::resolveReplaySubjectId(std::string const& choice) const {
    if (choice.empty()) return mRecordedLocalPlayerId;
    if (auto const raw = parseEntityChoice(choice)) return ActorUniqueID{*raw};
    std::lock_guard lock(mReplayPlayersMutex);
    auto const      it = mReplayPlayers.find(choice);
    if (it == mReplayPlayers.end()) return std::nullopt;
    return it->second.id;
}

void ReplaySession::refreshReplayEntityList() {
    std::vector<ReplayEntityInfo> entities;
    if (mReplayPlayer) {
        auto&                                    level = mReplayPlayer->getLevel();
        std::unordered_map<int64_t, std::string> playerUuids;
        {
            std::lock_guard lock(mReplayPlayersMutex);
            for (auto const& [uuid, record] : mReplayPlayers) playerUuids.emplace(record.id.rawID, uuid);
        }
        // Every recorded add, not only the actors that moved: a mob standing still has no render key yet.
        entities.reserve(mRecordedEntityIds.size());
        for (auto const& id : mRecordedEntityIds) {
            Actor* actor = level.fetchEntity(id, false);
            if (!actor || actor == mReplayPlayer || actor->mRemoved || !actor->isAlive()) continue;
            bool const player = actor->isPlayer();
            if (!player && !actor->hasCategory(ActorCategory::Mob)) continue;
            ReplayEntityInfo info;
            info.player = player;
            if (player && mRecordedLocalPlayerId && *mRecordedLocalPlayerId == id) {
                info.recorder = true; // the empty choice
            } else if (player) {
                auto const uuid = playerUuids.find(id.rawID);
                if (uuid == playerUuids.end()) continue;
                info.choice = uuid->second;
            } else {
                info.choice = entityChoice(id.rawID);
            }
            info.label = plainNameTag(actor->getNameTag());
            if (info.label.empty()) info.label = entityTypeLabel(*actor);
            auto const  position = actor->getPosition();
            auto const& box      = actor->getAABB();
            info.x               = position.x;
            info.y               = player ? box.min.y : position.y;
            info.z               = position.z;
            info.height          = box.max.y - box.min.y;
            if (!std::isfinite(info.x) || !std::isfinite(info.y) || !std::isfinite(info.z)) continue;
            if (!std::isfinite(info.height) || info.height <= 0.0f) info.height = 1.0f;
            entities.push_back(std::move(info));
        }
        auto const observer   = mReplayPlayer->getPosition();
        auto const distanceSq = [&observer](ReplayEntityInfo const& entity) {
            float const dx = entity.x - observer.x;
            float const dy = entity.y - observer.y;
            float const dz = entity.z - observer.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::sort(entities.begin(), entities.end(), [&](ReplayEntityInfo const& a, ReplayEntityInfo const& b) {
            return distanceSq(a) < distanceSq(b);
        });
    }
    std::lock_guard lock(mReplayEntitiesMutex);
    mReplayEntities = std::move(entities);
}

std::vector<ReplaySession::ReplayEntityInfo> ReplaySession::replayEntities() const {
    std::lock_guard lock(mReplayEntitiesMutex);
    return mReplayEntities;
}

std::string ReplaySession::replaySubjectLabel(std::string const& choice) const {
    {
        std::lock_guard lock(mReplayEntitiesMutex);
        for (auto const& entity : mReplayEntities) {
            if (entity.choice == choice) return entity.label;
        }
    }
    std::lock_guard lock(mReplayPlayersMutex);
    if (auto const it = mReplayPlayers.find(choice); it != mReplayPlayers.end() && !it->second.name.empty()) {
        return it->second.name;
    }
    return choice;
}

void ReplaySession::applyObserverSkin(SerializedSkinRef const& skin) {
    if (!mReplayPlayer || !skin.mSkinImpl) return;
    if (!mObserverOriginalSkin) {
        auto const& current = mReplayPlayer->mSkin;
        if (current && current->mSkinImpl) {
            mObserverOriginalSkin  = std::make_unique<SerializedSkinRef>();
            *mObserverOriginalSkin = *current;
        }
    }
    mReplayPlayer->updateSkin(skin, static_cast<int>(mReplayPlayer->getClientSubId()));
    mObserverSkinApplied = true;
}

void ReplaySession::restoreObserverSkin() {
    if (!mObserverSkinApplied || !mReplayPlayer || !mObserverOriginalSkin || !mObserverOriginalSkin->mSkinImpl) {
        mObserverSkinApplied = false;
        return;
    }
    mReplayPlayer->updateSkin(*mObserverOriginalSkin, static_cast<int>(mReplayPlayer->getClientSubId()));
    mObserverSkinApplied = false;
    getLogger().debug("replay observer skin restored");
}

void ReplaySession::restoreFollowedPlayerHud() {
    if (!mFollowedHudMirror || !mFollowedHudMirror->active) {
        mFollowedHudMirror.reset();
        return;
    }
    auto* observer = mReplayPlayer;
    auto  mirror   = std::move(mFollowedHudMirror);
    if (!observer) return;

    auto& inventory = observer->getInventory();
    for (int slot = 0; slot < 9; ++slot) inventory.setItem(slot, mirror->savedHotbar[static_cast<size_t>(slot)]);
    for (size_t index = 0; index < HudArmorSlots.size(); ++index) {
        observer->setArmor(HudArmorSlots[index], mirror->savedArmor[index]);
    }
    observer->setOffhandSlot(mirror->savedOffhand);
    if (observer->mInventory) (void)observer->mInventory->selectSlot(mirror->savedSelected, ContainerID::Inventory);
    for (auto const& attribute : mirror->savedAttributes) writeHudAttribute(*observer, attribute);
    getLogger().debug(
        "replay hud restored selected={} attributes={} diverted={}",
        mirror->savedSelected,
        mirror->savedAttributes.size(),
        mirror->diverted
    );
}

void ReplaySession::syncFollowedPlayerHud() {
    if (!mReplayPlayer) {
        restoreFollowedPlayerHud();
        return;
    }
    bool const showOther =
        visuals::isReplaySubjectViewDriven() && visuals::hasReplaySubject() && !visuals::isReplaySubjectRecorder();
    if (!showOther) {
        restoreFollowedPlayerHud();
        if (visuals::isReplaySubjectViewDriven() && visuals::isReplaySubjectRecorder() && mRecordedLocalPlayerId) {
            Actor* recorded = mReplayPlayer->getLevel().fetchEntity(*mRecordedLocalPlayerId, false);
            if (recorded && recorded->isPlayer()) {
                visuals::setReplayHudGameType(static_cast<int>(static_cast<Player*>(recorded)->getPlayerGameType()));
            }
        }
        return;
    }

    std::string const choice = replaySubjectChoice();
    if (choice.empty()) {
        restoreFollowedPlayerHud();
        return;
    }
    ActorUniqueID id;
    std::string   name;
    {
        std::lock_guard lock(mReplayPlayersMutex);
        auto const      found = mReplayPlayers.find(choice);
        if (found == mReplayPlayers.end() || !found->second.present || found->second.recorder) {
            restoreFollowedPlayerHud();
            return;
        }
        id   = found->second.id;
        name = found->second.name;
    }
    Actor* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
    if (!actor || actor == mReplayPlayer || !actor->isPlayer()) {
        restoreFollowedPlayerHud();
        return;
    }

    auto& subject = static_cast<Player&>(*actor);
    auto  mode    = subject.getPlayerGameType();
    if (mode == GameType::Spectator || mode == GameType::Default || mode == GameType::Undefined) {
        mode = GameType::Survival;
    }
    visuals::setReplayHudGameType(static_cast<int>(mode));

    if (!mFollowedHudMirror) mFollowedHudMirror = std::make_unique<FollowedHudMirror>();
    auto& mirror    = *mFollowedHudMirror;
    auto& inventory = mReplayPlayer->getInventory();
    if (!mirror.active || mirror.subjectId != id) {
        if (mirror.active) {
            for (auto& written : mirror.writtenHotbar) written = ItemStack{};
            for (auto& written : mirror.writtenArmor) written = ItemStack{};
            mirror.writtenOffhand  = ItemStack{};
            mirror.writtenSelected = -1;
        } else {
            for (int slot = 0; slot < 9; ++slot)
                mirror.savedHotbar[static_cast<size_t>(slot)] = inventory.getItem(slot);
            for (size_t index = 0; index < HudArmorSlots.size(); ++index) {
                mirror.savedArmor[index] = mReplayPlayer->getArmor(HudArmorSlots[index]);
            }
            mirror.savedOffhand  = mReplayPlayer->getOffhandSlot();
            mirror.savedSelected = mReplayPlayer->getSelectedItemSlot();
            mirror.savedAttributes.clear();
            for (auto const* attribute : hudAttributes()) {
                if (auto value = readHudAttribute(*mReplayPlayer, *attribute)) {
                    mirror.savedAttributes.push_back(std::move(*value));
                }
            }
        }
        mirror.subjectId  = id;
        mirror.active     = true;
        mirror.forceWrite = true;
        getLogger().debug("replay hud mirrored name={}", name);
    } else {
        // Anything that changed the observer behind the mirror's back is the recorder's own state; keep it for the
        // restore.
        for (int slot = 0; slot < 9; ++slot) {
            auto const& live = inventory.getItem(slot);
            if (!sameHudStack(live, mirror.writtenHotbar[static_cast<size_t>(slot)])) {
                mirror.savedHotbar[static_cast<size_t>(slot)] = live;
            }
        }
        for (size_t index = 0; index < HudArmorSlots.size(); ++index) {
            auto const& live = mReplayPlayer->getArmor(HudArmorSlots[index]);
            if (!sameHudStack(live, mirror.writtenArmor[index])) mirror.savedArmor[index] = live;
        }
        auto const& offhand = mReplayPlayer->getOffhandSlot();
        if (!sameHudStack(offhand, mirror.writtenOffhand)) mirror.savedOffhand = offhand;
        int const selected = mReplayPlayer->getSelectedItemSlot();
        if (selected != mirror.writtenSelected) mirror.savedSelected = selected;
    }

    // A remote player's own inventory only ever holds what the vanilla handler put there, and its selected slot stays
    // 0, so the hotbar is rebuilt from the recorded MobEquipment slots. The held item is always the latest truth.
    // Rewriting an unchanged slot every tick resets the slot's render state and makes the hotbar flicker.
    bool const          force   = std::exchange(mirror.forceWrite, false);
    auto const          tracked = gReplayHotbars.find(subject.getRuntimeID().rawID);
    ReplayHotbar const* hotbar  = tracked == gReplayHotbars.end() ? nullptr : &tracked->second;
    int const           selected =
        std::clamp(hotbar && hotbar->selected >= 0 ? hotbar->selected : subject.getSelectedItemSlot(), 0, 8);
    auto& subjectInventory = subject.getInventory();
    for (int slot = 0; slot < 9; ++slot) {
        ItemStack item = subjectInventory.getItem(slot);
        if (item.isNull() && hotbar) item = hotbar->items[static_cast<size_t>(slot)];
        if (slot == selected) item = subject.getCarriedItem();
        if (force || !sameHudStack(item, inventory.getItem(slot))) inventory.setItem(slot, item);
        mirror.writtenHotbar[static_cast<size_t>(slot)] = item;
    }
    for (size_t index = 0; index < HudArmorSlots.size(); ++index) {
        ItemStack item = subject.getArmor(HudArmorSlots[index]);
        if (force || !sameHudStack(item, mReplayPlayer->getArmor(HudArmorSlots[index]))) {
            mReplayPlayer->setArmor(HudArmorSlots[index], item);
        }
        mirror.writtenArmor[index] = item;
    }
    ItemStack offhand = subject.getOffhandSlot();
    if (force || !sameHudStack(offhand, mReplayPlayer->getOffhandSlot())) mReplayPlayer->setOffhandSlot(offhand);
    mirror.writtenOffhand = offhand;
    if ((force || mReplayPlayer->getSelectedItemSlot() != selected) && mReplayPlayer->mInventory) {
        (void)mReplayPlayer->mInventory->selectSlot(selected, ContainerID::Inventory);
    }
    mirror.writtenSelected = selected;

    // Hearts, hunger and the experience bar are drawn from the observer's attributes.
    for (auto const* attribute : hudAttributes()) {
        auto const wanted = readHudAttribute(subject, *attribute);
        if (!wanted) continue;
        auto const shown = readHudAttribute(*mReplayPlayer, *attribute);
        if (!force && shown && shown->current == wanted->current && shown->max == wanted->max) continue;
        writeHudAttribute(*mReplayPlayer, *wanted);
    }
}

void ReplaySession::rebindReplaySubject() {
    auto const loseSubject = [this](std::string const& missing) {
        visuals::setReplaySubject(std::nullopt);
        restoreObserverSkin();
        restoreFollowedPlayerHud();
        if (missing.empty() || mReplaySubjectGapChoice == missing) return;
        mReplaySubjectGapChoice = missing;
        getLogger().debug("replay subject pending choice={} reason=missing", missing);
    };
    if (!mReplayPlayer) {
        loseSubject("");
        return;
    }
    std::string const choice = replaySubjectChoice();
    ActorUniqueID     id;
    bool              recorder = false;
    std::string       name;
    std::string       uuid;
    auto const        entityRaw = parseEntityChoice(choice);
    if (choice.empty()) {
        if (!mRecordedLocalPlayerId) {
            loseSubject("");
            return;
        }
        id       = *mRecordedLocalPlayerId;
        recorder = true;
    } else if (entityRaw) {
        id = ActorUniqueID{*entityRaw};
    } else {
        std::lock_guard lock(mReplayPlayersMutex);
        auto const      it = mReplayPlayers.find(choice);
        if (it == mReplayPlayers.end()) {
            loseSubject(choice);
            return;
        }
        id       = it->second.id;
        recorder = it->second.recorder;
        name     = it->second.name;
        uuid     = it->first;
    }
    if (choice.empty()) {
        std::lock_guard lock(mReplayPlayersMutex);
        for (auto const& [playerUuid, record] : mReplayPlayers) {
            if (record.id != id) continue;
            name = record.name;
            uuid = playerUuid;
            break;
        }
    }
    // A followed entity that stays gone hands the view back to the recorder.
    auto const loseEntity = [&]() {
        loseSubject(choice);
        // A seek catch-up replays the entity's add later; only played ticks count toward the grace.
        if (mSeekTargetTick >= 0 || isSeekPending()) return;
        if (mReplaySubjectEntityMissingLastTick == mCurrentTick) return;
        mReplaySubjectEntityMissingLastTick = mCurrentTick;
        if (++mReplaySubjectEntityMissingTicks < EntitySubjectGraceTicks) return;
        getLogger().debug(
            "replay subject entity gone choice={} tick={} missingTicks={} fallback=recorder",
            choice,
            mCurrentTick,
            mReplaySubjectEntityMissingTicks
        );
        setReplaySubjectChoice({});
    };
    Actor* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
    if (!actor || actor == mReplayPlayer || (entityRaw && (actor->mRemoved || !actor->isAlive()))) {
        if (entityRaw) loseEntity();
        else loseSubject(choice);
        return;
    }
    auto const renderKey = replayEntityRenderKey(actor->getLevel(), id);
    if (!renderKey) {
        if (entityRaw) loseEntity();
        else loseSubject(choice);
        return;
    }
    mReplaySubjectEntityMissingTicks    = 0;
    mReplaySubjectEntityMissingLastTick = -1;
    if (!mEntityRenderKeys.contains(id)) {
        auto const pose = replayRenderPose(*actor);
        mEntityRenderKeys.insert_or_assign(id, *renderKey);
        visuals::queueReplayEntityPose(*renderKey, pose, pose);
    }
    bool const  player    = actor->isPlayer();
    float const eyeHeight = player ? 0.0f : entityEyeHeight(*actor);
    bool const  changed   = !visuals::isReplaySubject(*renderKey) || visuals::isReplaySubjectRecorder() != recorder
                      || visuals::isReplaySubjectPlayer() != player;
    visuals::setReplaySubjectRecorder(recorder);
    visuals::setReplaySubjectEntity(player, eyeHeight);
    visuals::setReplaySubject(*renderKey);
    mReplaySubjectGapChoice.clear();
    if (!changed) return;
    if (!player) {
        if (name.empty()) name = plainNameTag(actor->getNameTag());
        getLogger().debug(
            "replay subject bound entity={} type={} name={} eyeHeight={:.2f}",
            id.rawID,
            actor->getTypeName(),
            name,
            eyeHeight
        );
    } else {
        getLogger().debug("replay subject bound recorder={} name={} uuid={}", recorder, name, uuid);
    }
    bool applied = false;
    if (actor->isPlayer()) {
        auto&       subject = static_cast<Player&>(*actor);
        auto const& entries = subject.getLevel().getPlayerList();
        auto const  entry   = entries.find(subject.getUuid());
        if (entry != entries.end() && entry->second.mSkin->mSkinImpl) {
            applyObserverSkin(*entry->second.mSkin);
            getLogger().debug("replay observer skin applied name={} uuid={}", name, uuid);
            applied = true;
        } else if (auto const& subjectSkin = subject.mSkin; subjectSkin && subjectSkin->mSkinImpl) {
            // The player list can lag the actor. The actor skin is what third person already draws.
            applyObserverSkin(*subjectSkin);
            getLogger().debug("replay observer skin applied from actor name={} uuid={}", name, uuid);
            applied = true;
        }
    }
    if (!applied) restoreObserverSkin();
    updateReplaySubjectPosture(true);
}

void ReplaySession::updateReplaySubjectPosture(bool snap) {
    if (!mReplayPlayer || !visuals::hasReplaySubject()) return;
    auto const resolved = resolveReplaySubjectId(replaySubjectChoice());
    if (!resolved) return;
    ActorUniqueID const id    = *resolved;
    Actor*              actor = mReplayPlayer->getLevel().fetchEntity(id, false);
    if (!actor || actor == mReplayPlayer) return;
    // A growing baby or a resized slime changes its eyes; players use the recorded or measured offsets instead.
    if (!actor->isPlayer()) visuals::setReplaySubjectEntity(false, entityEyeHeight(*actor));
    visuals::noteReplaySubjectPosture(
        mCurrentTick,
        snap,
        visuals::ReplaySubjectPosture{
            actor->getStatusFlag(ActorFlags::Sneaking),
            actor->getStatusFlag(ActorFlags::Swimming),
            actor->getStatusFlag(ActorFlags::Crawling),
            actor->getStatusFlag(ActorFlags::Gliding),
            actor->getStatusFlag(ActorFlags::Sleeping),
            actor->isSleeping(),
        }
    );
}

// A snapshot wipes every player, and the chosen one may spawn a tick later or be absent from that snapshot.
void ReplaySession::refreshReplaySubjectBinding() {
    if (!mReplayPlayer || mPendingReplayDimension || mPendingSnapshotApply || mChunkInjectionPending) return;
    if (visuals::hasReplaySubject()) {
        mReplaySubjectGapChoice.clear();
        return;
    }
    rebindReplaySubject();
}

// Teleports the viewer to the local player: a pending request lands once, following repeats per frame.
bool ReplaySession::updateSubjectFollow() {
    // Player input owns the camera, and a started export drives it from the export timeline.
    bool const exporting = mExportTimelinePhase != ReplayExportTimelinePhase::Inactive;
    if (isFollowingRecordedPlayer() && (editor::input::isGameInputCaptured() || exporting)) {
        setFollowRecordedPlayer(false);
    }
    bool const following = !exporting && isFollowViewActive();
    if (exporting) mTeleportToSubjectPending = false;
    if (!following && !mTeleportToSubjectPending) return false;

    if (!mNetworkHandler || !isReplayWorldReady() || mPendingSnapshotApply || isSeekPending()) return following;

    auto const sampled = sampleSubjectFollowState();
    if (!sampled) return following;
    auto const& state    = *sampled;
    auto const  position = Vec3{state.x, state.y, state.z};
    bool const  oneShot  = std::exchange(mTeleportToSubjectPending, false);
    if (oneShot && !following) {
        parkObserverAtPose(state);
        getLogger().debug(
            "Observer teleported to the recorded player at tick {} ({:.2f}, {:.2f}, {:.2f})",
            mCurrentTick,
            position.x,
            position.y,
            position.z
        );
        return false;
    }

    Vec3 const     feetPosition{state.x, state.y, state.z};
    Vec2 const     rotation{state.pitch, state.yaw};
    ChunkPos const chunk{feetPosition.x, feetPosition.z};
    bool const serverSyncNeeded = oneShot || !mLastObserverServerSyncChunk || mLastObserverServerSyncChunk->x != chunk.x
                               || mLastObserverServerSyncChunk->z != chunk.z;
    forceFirstPersonCamera();
    teleportReplayPlayer(feetPosition, rotation);
    cancelNativeMovementInterpolation(*mReplayPlayer, feetPosition, rotation, rotation.y);
    if (serverSyncNeeded) syncObserverServerPosition(feetPosition, rotation);
    return true;
}

std::optional<keyframe::CameraRenderState> ReplaySession::sampleSubjectFollowState() {
    // The entity pose trails the camera by one tick; the camera time covers tick 0.
    auto time = getEntityRenderSampleTime();
    if (!time) time = getCameraRenderSampleTime();
    if (!time) return std::nullopt;
    auto const pose = visuals::sampleReplaySubjectPose(*time);
    if (!pose) return std::nullopt;
    auto const& position = pose->position;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
        || !std::isfinite(pose->yaw) || !std::isfinite(pose->pitch)
        || (position.x == 0.0f && position.y == 0.0f && position.z == 0.0f)) {
        return std::nullopt;
    }
    // The recorded player's position is already its eye position; the recorded FOV carries the spyglass zoom.
    auto const fov = visuals::sampleReplaySubjectFov(*time);
    // The hand and vignette hooks read the sleep state sampled here.
    (void)visuals::sampleReplaySubjectViewWeight(*time);
    return keyframe::CameraRenderState{
        position.x,
        position.y,
        position.z,
        pose->yaw,
        pose->pitch,
        0.0f,
        fov && *fov > 1.0f && *fov < 179.0f ? *fov : 70.0f,
    };
}

// Server corrections land during the client update, after the previous frame's pin; pin again right before drawing.
std::optional<keyframe::CameraRenderState> ReplaySession::pinFollowObserverForRender() {
    if (!isFollowViewActive() || !mReplayPlayer || !mReplayWorldJoined || !mNetworkHandler
        || mExportTimelinePhase != ReplayExportTimelinePhase::Inactive || mPendingReplayDimension) {
        mFollowHeldPose.reset();
        return std::nullopt;
    }
    // The tick stops while chunks or a snapshot stream in, but frames keep coming: keep the last pose.
    if (!isReplayWorldReady() || mPendingSnapshotApply || isSeekPending()) return mFollowHeldPose;

    auto state = sampleSubjectFollowState();
    if (!state) return mFollowHeldPose;
    cancelNativeMovementInterpolation(
        *mReplayPlayer,
        Vec3{state->x, state->y, state->z},
        Vec2{state->pitch, state->yaw},
        state->yaw
    );
    // The observer stays on the eyes so chunks and the server copy follow; only the drawn camera moves back.
    // The sample above refreshed the sleep weight, so a subject in bed already reads as first person here.
    if (isFollowViewThirdPerson() && visuals::isReplaySubjectThirdPerson()) {
        state = thirdPersonFollowPose(*mReplayPlayer, *state);
    }
    mFollowHeldPose = state;
    return state;
}

void ReplaySession::enforceReplayWorldTime() {
    if (!mActive || !mReplayTime || !mReplayPlayer) return;
    auto& level = mReplayPlayer->getLevel();
    if (level.getTime() != *mReplayTime) level.setTime(*mReplayTime);
}

void ReplaySession::teleportReplayPlayer(Vec3 const& feetPosition, Vec2 const& rotation) {
    if (!mReplayPlayer || !mReplayWorldJoined || !mNetworkHandler) return;
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::MovePlayer);
    if (!packet || !packet->mHandler) return;
    auto& move          = static_cast<MovePlayerPacket&>(*packet);
    move.mPlayerID      = mReplayPlayer->getRuntimeID();
    move.mPos           = feetPosition;
    move.mRot           = rotation;
    move.mYHeadRot      = rotation.y;
    move.mResetPosition = PlayerPositionModeComponent::PositionMode::Teleport;
    move.mTeleportData  = MovePlayerTeleportData{0, -1};
    move.mOnGround      = true;
    mInjectingPacket.store(packet.get(), std::memory_order_release);
    InjectionReset reset{mInjectingPacket};
    packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    mObserverServerPositionDirty = true;
}

void ReplaySession::syncObserverServerPosition(Vec3 const& feetPosition, Vec2 const& rotation) {
    if (!mReplayPlayer || !mReplayWorldJoined || mPendingReplayDimension) return;
    mLastObserverServerSyncChunk   = ChunkPos{feetPosition.x, feetPosition.z};
    auto const playerUuid          = mReplayPlayer->getUuid();
    auto const replayLevelId       = mReplayLevelId;
    auto const dimension           = mReplayPlayer->getDimensionId();
    auto const syncEpoch           = mObserverServerSyncEpoch.load(std::memory_order_acquire);
    auto*      syncEpochCounter    = &mObserverServerSyncEpoch;
    auto const syncSequence        = mObserverServerSyncSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto*      syncSequenceCounter = &mObserverServerSyncSequence;
    Vec3 const serverPosition{feetPosition.x, feetPosition.y - kServerEyeHeight, feetPosition.z};
    mObserverServerPositionDirty = false;
    ll::thread::ServerThreadExecutor::getDefault().execute([playerUuid,
                                                            replayLevelId,
                                                            serverPosition,
                                                            rotation,
                                                            dimension,
                                                            syncEpoch,
                                                            syncEpochCounter,
                                                            syncSequence,
                                                            syncSequenceCounter] {
        if (syncEpochCounter->load(std::memory_order_acquire) != syncEpoch
            || syncSequenceCounter->load(std::memory_order_acquire) != syncSequence) {
            return;
        }
        auto level = ll::service::getLevel();
        if (!level || level->getLevelId() != replayLevelId) return;
        auto* player = level->getPlayer(playerUuid);
        if (!player) return;
        if (syncEpochCounter->load(std::memory_order_acquire) != syncEpoch
            || syncSequenceCounter->load(std::memory_order_acquire) != syncSequence) {
            return;
        }
        player->teleport(serverPosition, dimension, rotation);
    });
}

void ReplaySession::updateObserverPreview() {
    if (!mReplayPlayer || !mReplayWorldJoined) return;
    updatePreviewShot();
    if (updateSubjectFollow()) {
        mPendingObserverPark.reset();
        return;
    }
    parkObserverAfterFollow();
    if (mPendingObserverPark && mNetworkHandler && isReplayWorldReady() && !mPendingSnapshotApply && !isSeekPending()
        && !mPendingReplayDimension && mExportTimelinePhase == ReplayExportTimelinePhase::Inactive) {
        auto const pose = *std::exchange(mPendingObserverPark, std::nullopt);
        parkObserverAtPose(pose);
        return;
    }
    if (mIsPaused) {
        if (!mPendingReplayDimension) updatePausedObserverPreview();
        return;
    }
    if (mPendingReplayDimension) return;
    if (!mNetworkHandler) return;
    if (!keyframe::hasCameraTimeline(keyframe::CameraTimelineSource::Preview)) {
        releasePreviewObserver();
        return;
    }
    auto const time = getCameraRenderSampleTime();
    if (!time) return;
    // Republishing (a track was toggled or edited) invalidates the cached pose, unlike merely leaving the range.
    auto const timelineGeneration = keyframe::previewTimelineGeneration();
    if (mObserverPreviewGeneration != timelineGeneration) {
        mObserverPreviewGeneration = timelineGeneration;
        releasePreviewObserver();
    }

    auto const sample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Preview, *time);
    if (!sample) {
        // Leaving the range: pin the observer and server to the last in-range pose.
        releasePreviewObserver();
        return;
    }
    mObserverPreviewInRange = true;

    // First-person spectator: camera sits at the feet, so the pose is the feet position.
    Vec3 const feetPosition{sample->state.x, sample->state.y, sample->state.z};
    Vec2 const rotation{sample->state.pitch, sample->state.yaw};
    mLastObserverPreviewFeet     = feetPosition;
    mLastObserverPreviewRotation = rotation;
    moveObserverTo(feetPosition, rotation);
}

void ReplaySession::moveObserverTo(Vec3 const& feetPosition, Vec2 const& rotation) {
    forceFirstPersonCamera();
    ChunkPos const cameraChunk{feetPosition.x, feetPosition.z};
    bool const     serverSyncNeeded = !mLastObserverServerSyncChunk || mLastObserverServerSyncChunk->x != cameraChunk.x
                               || mLastObserverServerSyncChunk->z != cameraChunk.z;
    teleportReplayPlayer(feetPosition, rotation);
    cancelNativeMovementInterpolation(*mReplayPlayer, feetPosition, rotation, rotation.y);
    if (serverSyncNeeded) syncObserverServerPosition(feetPosition, rotation);
}

void ReplaySession::syncPreviewObserverForTick(int targetTick, bool onlyIfMoved) {
    if (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive || !mReplayPlayer || !mReplayWorldJoined
        || !mNetworkHandler || mPendingReplayDimension) {
        return;
    }
    if (isFollowViewActive() || editor::input::isGameInputCaptured()) return;
    auto const timeline = keyframe::currentCameraTimeline(keyframe::CameraTimelineSource::Preview);
    if (!timeline) return;

    auto const moveIfNeeded = [&](Vec3 const& feetPosition, Vec2 const& rotation) {
        if (onlyIfMoved && mReplayPlayer->getPosition().distanceToSqr(feetPosition) <= 1e-4f) return;
        moveObserverTo(feetPosition, rotation);
    };

    auto const sampleTime = visuals::ReplaySampleTime::fromPreview(targetTick, 0.0f);
    if (sampleTime) {
        if (auto const sample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Preview, *sampleTime)) {
            moveIfNeeded(
                Vec3{sample->state.x, sample->state.y, sample->state.z},
                Vec2{sample->state.pitch, sample->state.yaw}
            );
            return;
        }
    }

    auto const shot = timeline->shotAt(targetTick);
    if (!shot || shot.kind() == state::editing::model::CameraTrackKind::Keyframe) return;
    auto const id = resolveReplaySubjectId(shot.subject);
    if (!id) return;
    auto* actor = mReplayPlayer->getLevel().fetchEntity(*id, false);
    if (!actor || actor == mReplayPlayer) return;
    moveIfNeeded(actor->getPosition(), actor->getRotation());
}

void ReplaySession::syncSeekObserver() {
    if (!mReplayPlayer || !mReplayWorldJoined || !mNetworkHandler || mPendingReplayDimension) return;
    if (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive) {
        if (!mExportCameraViewpoint) return;
        Vec3 const target{mExportCameraViewpoint->x, mExportCameraViewpoint->y, mExportCameraViewpoint->z};
        if (mReplayPlayer->getPosition().distanceToSqr(target) > 1e-4f) {
            updateExportObserver(*mExportCameraViewpoint);
        }
        return;
    }
    syncPreviewObserverForTick(mSeekTargetTick, true);
}

void ReplaySession::markReplayTickAdvanced() noexcept {
    mTickAdvancedAt.store(std::chrono::steady_clock::now(), std::memory_order_release);
    mFrozenPreviewPartial.store(-1.0f, std::memory_order_release);
}

// Backdate the tick start by the frozen fraction so the first resumed frame keeps the paused pose.
void ReplaySession::resumePreviewClockFromFrozenPartial() noexcept {
    auto const frozen = mFrozenPreviewPartial.exchange(-1.0f, std::memory_order_acq_rel);
    auto const speed  = mPlaybackSpeed > 0.0f ? mPlaybackSpeed : 1.0f;
    auto const offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(std::max(0.0f, frozen) / (20.0f * speed))
    );
    mTickAdvancedAt.store(std::chrono::steady_clock::now() - offset, std::memory_order_release);
}

float ReplaySession::previewPartialTick() const noexcept {
    auto const frozen = mFrozenPreviewPartial.load(std::memory_order_acquire);
    if (frozen >= 0.0f) return frozen;

    auto const advancedAt = mTickAdvancedAt.load(std::memory_order_acquire);
    if (advancedAt == SteadyTimePoint{}) return 0.0f;

    auto const speed = mPlaybackSpeed > 0.0f ? mPlaybackSpeed : 1.0f;
    auto const elapsed =
        std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - advancedAt).count();
    constexpr float SecondsPerTick = 1.0f / 20.0f;
    return std::clamp(elapsed * speed / SecondsPerTick, 0.0f, 1.0f);
}

// Native render alpha uses a different clock and can move replay sampling backwards.
std::optional<visuals::ReplaySampleTime> ReplaySession::getCameraRenderSampleTime() const noexcept {
    if (!mActive || !mReplayWorldJoined) return std::nullopt;

    auto const appliedTick = std::max(0, mCurrentTick);
    return visuals::ReplaySampleTime::fromPreview(appliedTick, previewPartialTick());
}

// Pose history spans [appliedTick - 1, appliedTick], so entity sampling trails the camera by one tick.
std::optional<visuals::ReplaySampleTime> ReplaySession::getEntityRenderSampleTime() const noexcept {
    if (!mActive || !mReplayWorldJoined) return std::nullopt;

    auto const appliedTick = std::max(0, mCurrentTick);
    if (appliedTick == 0) return std::nullopt;
    return visuals::ReplaySampleTime::fromPreview(appliedTick - 1, previewPartialTick());
}

bool ReplaySession::beginExportTimeline(int startTick) {
    if (!mActive || mReplayFailed || mExportTimelinePhase != ReplayExportTimelinePhase::Inactive) return false;

    // The export warm-up waits for a menu-free scene, so a preview from before the export must not stay open.
    closeScreenPreview("exportBegin");
    popReplayScreen();
    clearPreviewShot();
    setFollowRecordedPlayer(false);
    mTeleportToSubjectPending = false;
    startTick                 = std::clamp(startTick, 0, getTotalTicks());
    mIsPaused                 = true;
    mPlaybackTickAccumulator  = 0.0f;

    mRequestedSeekTick.store(startTick, std::memory_order_release);
    mSeekTargetTick         = -1;
    mExportSeekRequested    = true;
    mSnapMovementDuringSeek = false;
    mExportTargetTick       = startTick;
    mExportTimelinePhase    = ReplayExportTimelinePhase::Initializing;
    mObserverServerSyncEpoch.fetch_add(1, std::memory_order_acq_rel);
    mObserverServerPositionDirty = false;
    mLastObserverServerSyncChunk.reset();
    getLogger().debug("Export timeline initialization queued (startTick={}, currentTick={})", startTick, mCurrentTick);
    return true;
}

void ReplaySession::setExportCameraViewpoint(std::optional<ReplayCameraViewpoint> viewpoint) noexcept {
    if (viewpoint
        && (!std::isfinite(viewpoint->x) || !std::isfinite(viewpoint->y) || !std::isfinite(viewpoint->z)
            || !std::isfinite(viewpoint->pitch) || !std::isfinite(viewpoint->yaw) || !std::isfinite(viewpoint->roll)
            || !std::isfinite(viewpoint->fov) || viewpoint->fov <= 1.0f || viewpoint->fov >= 179.0f)) {
        viewpoint.reset();
    }
    mExportCameraViewpoint = viewpoint;
}

std::optional<ReplayCameraViewpoint> ReplaySession::currentCameraViewpoint() const noexcept {
    if (!isReadyForExport()) return std::nullopt;
    auto const            position = mReplayPlayer->getPosition();
    auto const            rotation = mReplayPlayer->getRotation();
    ReplayCameraViewpoint viewpoint{position.x, position.y, position.z, rotation.x, rotation.y, 0.0f, 70.0f};
    if (!std::isfinite(viewpoint.x) || !std::isfinite(viewpoint.y) || !std::isfinite(viewpoint.z)
        || !std::isfinite(viewpoint.pitch) || !std::isfinite(viewpoint.yaw)) {
        return std::nullopt;
    }
    return viewpoint;
}

std::optional<ReplayCameraViewpoint> ReplaySession::exportCameraViewpoint() const noexcept {
    return mExportCameraViewpoint;
}

void ReplaySession::updateExportObserver(ReplayCameraViewpoint const& viewpoint) {
    if (mExportTimelinePhase == ReplayExportTimelinePhase::Inactive || !mReplayPlayer || !mReplayWorldJoined
        || !mNetworkHandler || mPendingReplayDimension || !std::isfinite(viewpoint.x) || !std::isfinite(viewpoint.y)
        || !std::isfinite(viewpoint.z) || !std::isfinite(viewpoint.pitch) || !std::isfinite(viewpoint.yaw)) {
        return;
    }

    moveObserverTo(Vec3{viewpoint.x, viewpoint.y, viewpoint.z}, Vec2{viewpoint.pitch, viewpoint.yaw});
}

std::unique_ptr<visuals::ScopedReplayEntityPose>
ReplaySession::createReplayEntityRenderScope(visuals::ReplaySampleTime const& sample) {
    if (!sample.isValid() || !mActive || !mReplayWorldJoined || !mWorldReady || !refreshReplayPlayer()) return {};

    std::vector<visuals::EntityRenderTarget> targets;
    targets.reserve(mEntityRenderKeys.size());
    for (auto const& [id, key] : mEntityRenderKeys) {
        targets.push_back({key, mReplayPlayer->getLevel().fetchEntity(id, false)});
    }
    return visuals::createReplayEntityRenderScope(targets, sample);
}

ReplayExportTickState ReplaySession::prepareExportTick(int targetTick) {
    if (!mActive) return ReplayExportTickState::Unavailable;
    if (mReplayFailed) return ReplayExportTickState::Failed;
    if (mExportTimelinePhase == ReplayExportTimelinePhase::Inactive) return ReplayExportTickState::Unavailable;

    targetTick               = std::clamp(targetTick, 0, getTotalTicks());
    mIsPaused                = true;
    mPlaybackTickAccumulator = 0.0f;

    if (mExportTimelinePhase == ReplayExportTimelinePhase::Initializing) {
        if (targetTick != mExportTargetTick) return ReplayExportTickState::Invalid;
    } else {
        if (targetTick < mExportTargetTick || targetTick < mCurrentTick) return ReplayExportTickState::Invalid;
        mExportTargetTick = targetTick;

        if (mRequestedSeekTick.load(std::memory_order_acquire) >= 0 || mSeekTargetTick >= 0) {
            getLogger().error("A normal replay seek was requested while continuous export was active");
            return ReplayExportTickState::Failed;
        }
    }

    if (!mReplayWorldJoined || !mWorldReady || !mNetworkHandler) return ReplayExportTickState::Waiting;

    if (mExportTimelinePhase == ReplayExportTimelinePhase::Initializing) {
        auto const requestedTick = mRequestedSeekTick.load(std::memory_order_acquire);
        if (requestedTick >= 0) {
            mExportSeekRequested = true;
            if (requestedTick != targetTick) mRequestedSeekTick.store(targetTick, std::memory_order_release);
            return ReplayExportTickState::Waiting;
        }
        if (mSeekTargetTick >= 0 || mPendingReplayDimension || mPendingSnapshotApply || mChunkInjectionPending) {
            return ReplayExportTickState::Waiting;
        }
    } else if (mPendingReplayDimension || mPendingSnapshotApply || mChunkInjectionPending) {
        return ReplayExportTickState::Waiting;
    }

    if (hasPendingReplayReaderBoundary()) return ReplayExportTickState::Waiting;
    if (mCurrentTick == targetTick) return ReplayExportTickState::Ready;
    if (mReaderIndex >= mReaders.size() && mCurrentTick < targetTick) return ReplayExportTickState::Failed;

    if (mExportTimelinePhase == ReplayExportTimelinePhase::Initializing) {
        mExportSeekRequested = true;
        mRequestedSeekTick.store(targetTick, std::memory_order_release);
    }
    return ReplayExportTickState::Waiting;
}

bool ReplaySession::finishExportTimelineInitialization() {
    if (mExportTimelinePhase != ReplayExportTimelinePhase::Initializing || !mActive || mReplayFailed) return false;
    if (mCurrentTick != mExportTargetTick || mSeekTargetTick >= 0
        || mRequestedSeekTick.load(std::memory_order_acquire) >= 0 || mPendingReplayDimension || mPendingSnapshotApply
        || mChunkInjectionPending || hasPendingReplayReaderBoundary()) {
        return false;
    }

    mExportSeekRequested    = false;
    mSnapMovementDuringSeek = false;
    mExportTimelinePhase    = ReplayExportTimelinePhase::Continuous;
    syncScreenPreviewAfterSeek();
    return true;
}

void ReplaySession::endExportTimeline() {
    bool const wasActive = mExportTimelinePhase != ReplayExportTimelinePhase::Inactive;
    mRequestedSeekTick.store(-1, std::memory_order_release);
    mSeekTargetTick         = -1;
    mExportSeekRequested    = false;
    mSnapMovementDuringSeek = false;
    mExportTargetTick       = -1;
    mExportTimelinePhase    = ReplayExportTimelinePhase::Inactive;
    mExportCameraViewpoint.reset();
    if (wasActive) {
        mObserverServerSyncEpoch.fetch_add(1, std::memory_order_acq_rel);
        mObserverServerPositionDirty = false;
        mLastObserverServerSyncChunk.reset();
        if (mReplayPlayer && mReplayWorldJoined) {
            auto const position = mReplayPlayer->getPosition();
            auto const rotation = mReplayPlayer->getRotation();
            syncObserverServerPosition(position, rotation);
        }
    }
}

int ReplaySession::getTotalTicks() const { return std::max(0, mMeta.totalTicks); }

void ReplaySession::adjustPlaybackSpeed(int direction) {
    if (!mActive || direction == 0) return;

    size_t currentIndex = 0;
    for (size_t index = 1; index < PlaybackSpeeds.size(); ++index) {
        if (std::abs(PlaybackSpeeds[index] - mPlaybackSpeed)
            < std::abs(PlaybackSpeeds[currentIndex] - mPlaybackSpeed)) {
            currentIndex = index;
        }
    }

    auto const nextIndex     = static_cast<size_t>(std::clamp(
        static_cast<int>(currentIndex) + (direction < 0 ? -1 : 1),
        0,
        static_cast<int>(PlaybackSpeeds.size() - 1)
    ));
    mPlaybackSpeed           = PlaybackSpeeds[nextIndex];
    mPlaybackTickAccumulator = 0.0f;
    getLogger().debug("Replay speed set to {:.2f}x", mPlaybackSpeed);
}

void ReplaySession::setPlaybackSpeed(float speed) {
    if (!mActive || !std::isfinite(speed)) return;

    // Snap to the same ladder the stepping shortcuts walk, so both entry points agree.
    size_t nearest = 0;
    for (size_t index = 1; index < PlaybackSpeeds.size(); ++index) {
        if (std::abs(PlaybackSpeeds[index] - speed) < std::abs(PlaybackSpeeds[nearest] - speed)) nearest = index;
    }
    if (std::abs(PlaybackSpeeds[nearest] - mPlaybackSpeed) < 0.0001f) return;
    mPlaybackSpeed           = PlaybackSpeeds[nearest];
    mPlaybackTickAccumulator = 0.0f;
    getLogger().debug("Replay speed set to {:.2f}x", mPlaybackSpeed);
}

std::span<float const> ReplaySession::playbackSpeeds() noexcept { return PlaybackSpeeds; }

void ReplaySession::finishSeek() {
    // Export seeks never tick the level during catch-up, so they always drop the particles it piled up.
    if (mExportSeekRequested) mSeekParticlesStale = true;
    mSeekTargetTick         = -1;
    mExportSeekRequested    = false;
    mSnapMovementDuringSeek = false;
    mSeekFinishCount.fetch_add(1, std::memory_order_acq_rel);
    settleAfterSeek();
    // Catch-up ticks bypassed the preview clock, so restart it at the tick the seek landed on.
    markReplayTickAdvanced();
    if (mIsPaused) mFrozenPreviewPartial.store(0.0f, std::memory_order_release);
    syncScreenPreviewAfterSeek();
}

// Per-action snapping misses entities that never moved during catch-up, so settle every recorded one here.
void ReplaySession::settleAfterSeek() {
    if (mSeekParticlesStale) clearReplayParticles();
    mSeekParticlesStale   = false;
    mSeekParticlesCleared = false;
    if (!mReplayPlayer || !mReplayWorldJoined) return;

    auto& level = mReplayPlayer->getLevel();
    for (auto const& id : mRecordedEntityIds) {
        auto* actor = level.fetchEntity(id, false);
        if (!actor || actor == mReplayPlayer) continue;
        auto const rotation = actor->getRotation();
        cancelNativeMovementInterpolation(*actor, actor->getPosition(), rotation, rotation.y);
    }
}

void ReplaySession::beginSeek(int targetTick) {
    targetTick = std::clamp(targetTick, 0, getTotalTicks());
    if (mReaders.empty()) return;
    if (!refreshReplayPlayer()) throw std::runtime_error("Replay player is unavailable while seeking");

    mEntityRenderKeys.clear();
    visuals::clearReplayEntityPoses();
    visuals::setReplaySubject(std::nullopt);

    size_t selectedReader = mReaders.size() - 1;
    int    selectedStart  = 0;
    int    chunkStart     = 0;
    size_t chunkIndex     = 0;
    for (auto const& [_, chunkMeta] : mMeta.chunks) {
        int const chunkEnd = chunkStart + std::max(0, chunkMeta.duration);
        if (targetTick < chunkEnd || chunkIndex + 1 == mReaders.size()) {
            selectedReader = chunkIndex;
            selectedStart  = chunkStart;
            break;
        }
        chunkStart = chunkEnd;
        ++chunkIndex;
    }

    bool const exportSeek    = mExportSeekRequested;
    mIsPaused                = true;
    mPlaybackTickAccumulator = 0.0f;
    mSeekTargetTick          = targetTick;
    mSeekWindowEntered       = false;
    mSeekTickResuming        = false;
    mSnapMovementDuringSeek  = !exportSeek || mExportTimelinePhase == ReplayExportTimelinePhase::Initializing;
    mSeekParticlesCleared    = false;
    mSeekParticlesStale      = false;
    if (selectedReader >= mSnapshotContexts.size()) {
        throw std::runtime_error("Replay seek snapshot context index is out of range");
    }

    auto const currentDimension      = mReplayPlayer->getDimensionId();
    auto const targetDimension       = DimensionType{mSnapshotContexts[selectedReader].dimensionId};
    bool const changesDimension      = currentDimension != targetDimension;
    bool       crossesForcedSnapshot = false;
    if (selectedReader != mReaderIndex) {
        size_t const firstReader = std::min(selectedReader, mReaderIndex);
        size_t const lastReader  = std::max(selectedReader, mReaderIndex);
        size_t       readerIndex = 0;
        for (auto const& [_, chunkMeta] : mMeta.chunks) {
            if (readerIndex > firstReader && readerIndex <= lastReader && chunkMeta.forcePlaySnapshot) {
                crossesForcedSnapshot = true;
                break;
            }
            ++readerIndex;
        }
    }
    bool const followRecordedPlayer = changesDimension || crossesForcedSnapshot;
    bool const forceExportSnapshot  = exportSeek && mExportTimelinePhase == ReplayExportTimelinePhase::Initializing;
    if (targetTick >= mCurrentTick && !followRecordedPlayer && !forceExportSnapshot) {
        if (exportSeek) mSnapMovementDuringSeek = mExportTimelinePhase == ReplayExportTimelinePhase::Initializing;
        getLogger().debug(
            "Fast-forwarding replay from tick {} to tick {} without reloading snapshots",
            mCurrentTick,
            targetTick
        );
        rebindReplaySubject();
        return;
    }

    if (forceExportSnapshot) {
        getLogger().debug(
            "Rebuilding replay snapshot for export initialization at tick {} (snapshotTick={})",
            targetTick,
            selectedStart
        );
    }

    // A reloaded world keeps the particles of the place it left, so they are dropped even when the landing ticks
    // nothing. The same interval does not close: the screen stays until the landing tick leaves it.
    auto const* landing    = recordedScreenOpenAt(targetTick);
    bool const  sameScreen = mScreenPreview && landing && mScreenPreview->shows(*landing);
    if (changesDimension || forceExportSnapshot || !sameScreen) closeScreenPreview("seek");
    mSeekParticlesStale = true;
    mReaderIndex        = selectedReader;
    mCurrentTick        = selectedStart;
    integration::pushReplayClock(mCurrentTick, true);
    applySnapshot(*mReaders[mReaderIndex], followRecordedPlayer);
    getLogger()
        .debug("Seeking replay to tick {} from snapshot {} at tick {}", targetTick, selectedReader, selectedStart);
}

void ReplaySession::tick() {
    if (!mActive) return;
    try {
        if (!mReplayWorldJoined || !mNetworkHandler || !refreshReplayPlayer()) return;
        // The level stays live on every early return; only a settled pause or slow-motion gap freezes it.
        struct LevelFreezeScope {
            ReplaySession& session;
            bool           freeze{};
            ~LevelFreezeScope() { session.syncLevelSimPause(freeze); }
        } levelFreeze{*this};
        // First-person keeps the observer camera offset stable.
        forceFirstPersonCamera();
        if (mReplayTime && mReplayPlayer) mReplayPlayer->getLevel().setTime(*mReplayTime);
        updateScreenPreview();
        syncFollowedPlayerHud();

        if (mPendingSnapshotApply) {
            if (mPendingReplayDimension) return;
            auto pending = *mPendingSnapshotApply;
            mPendingSnapshotApply.reset();
            if (pending.readerIndex >= mReaders.size()) {
                throw std::runtime_error("Pending replay snapshot index is out of range");
            }
            if (pending.dimensionGeneration != 0 && pending.dimensionGeneration != mCompletedDimensionGeneration) {
                throw std::runtime_error("Pending replay snapshot does not match the completed dimension transition");
            }
            applySnapshot(*mReaders[pending.readerIndex], pending.followRecordedPlayer, pending.serverPlayerRelocated);
            return;
        }

        if (mPendingReplayDimension) return;

        if (!mWorldReady) {
            onWorldReady();
            return;
        }
        if (mChunkInjectionPending) {
            if (!tryFinishChunkInjection()) {
                if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
                return;
            }
            if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
        }

        auto const pendingRequestedSeek = mRequestedSeekTick.load(std::memory_order_acquire);
        if (hasPendingReplayReaderBoundary() && pendingRequestedSeek < 0) {
            bool const stopAtEnd = mExportTimelinePhase == ReplayExportTimelinePhase::Inactive && mSeekTargetTick < 0;
            (void)advanceReplayReader(stopAtEnd);
            if (mChunkInjectionPending || mPendingSnapshotApply || mPendingReplayDimension) return;
        }

        int const requestedSeek = mRequestedSeekTick.exchange(-1, std::memory_order_acq_rel);
        if (requestedSeek >= 0) {
            beginSeek(requestedSeek);
            if (mPendingSnapshotApply || mPendingReplayDimension) return;
        }
        if (mPendingSnapshotApply || mPendingReplayDimension) return;
        // A seek cleared from outside, such as by an export, leaves nothing to land the held chunks.
        if (mSeekChunksDeferred && mSeekTargetTick < 0) {
            flushSeekChunks();
            if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
        }
        // Yielding the tick that finishes a snapshot lets the client rebuild its players before catch-up resumes.
        if (mChunkInjectionPending) return;

        refreshReplayPlayerRoster();
        refreshReplaySubjectBinding();
        refreshReplayEntityList();

        if (mSeekTargetTick >= 0) {
            auto const catchUpStart     = std::chrono::steady_clock::now();
            auto const budget           = mExportSeekRequested ? SeekCatchUpBudget : SeekFrameBudget;
            auto const deadline         = catchUpStart + budget;
            bool       advancedThisCall = false;
            while (mCurrentTick < mSeekTargetTick) {
                // Draining here instead of returning keeps the rest of this client tick usable for replay.
                if (mChunkInjectionPending) {
                    auto const injectDeadline =
                        mExportSeekRequested
                            ? deadline
                            : std::min(deadline, std::chrono::steady_clock::now() + OuterChunkInjectionBudget * 4);
                    if (!tryFinishChunkInjection(injectDeadline)) {
                        if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
                        break;
                    }
                    if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
                }
                if (mPendingSnapshotApply || mPendingReplayDimension) break;
                // The terrain loaded so far is the landing's base, so it lands once as the window opens. Chunks sent
                // inside the window stay held for the final flush instead of each stopping catch-up for a frame.
                if (!mSeekWindowEntered && mCurrentTick >= mSeekTargetTick - SeekParticleWindowTicks) {
                    // The flush and the plan after it each take about a frame's budget, so a frame already half spent
                    // leaves them to the next one.
                    auto const halfSpent = [&] {
                        return std::chrono::steady_clock::now() - catchUpStart >= budget / 2;
                    };
                    if (mSeekChunksDeferred && advancedThisCall && !mExportSeekRequested && halfSpent()) break;
                    mSeekWindowEntered = true;
                    if (mSeekChunksDeferred) {
                        flushSeekChunks();
                        if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
                        if (!mExportSeekRequested && halfSpent()) break;
                        continue;
                    }
                }
                refreshReplayPlayerRoster();
                refreshReplaySubjectBinding();
                syncSeekObserver();
                // A tick resumed after its entity's chunks landed already ticked the level the first time through.
                bool const resumingTick = std::exchange(mSeekTickResuming, false);
                // Export seeks keep the clear at the landing frame so their first frames stay unchanged.
                bool const inParticleWindow =
                    !mExportSeekRequested && mCurrentTick >= mSeekTargetTick - SeekParticleWindowTicks;
                if (inParticleWindow) {
                    if (!mSeekParticlesCleared) {
                        mSeekParticlesCleared = true;
                        // Ticks skipped before the window left their particle events piled up at age zero.
                        if (mSeekParticlesStale) {
                            clearReplayParticles();
                            mSeekParticlesStale = false;
                        }
                        // A paused level would tick its particles as frozen.
                        syncLevelSimPause(false);
                    }
                    // Like normal playback the level ticks before each replay tick, so the landing is not aged past it.
                    if (!resumingTick) {
                        if (runtime::runNativeLevelTick()) ageReplayParticles();
                        else mSeekParticlesStale = true;
                    }
                } else {
                    mSeekParticlesStale = true;
                }
                if (!advanceReplayTick(false)) {
                    getLogger().warn("Replay ended at tick {} while seeking to tick {}", mCurrentTick, mSeekTargetTick);
                    flushSeekChunks();
                    if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
                    finishSeek();
                    return;
                }
                advancedThisCall = true;
                if (std::chrono::steady_clock::now() >= deadline) break;
            }

            if (mCurrentTick >= mSeekTargetTick) {
                getLogger().debug("Replay seek completed at tick {}", mCurrentTick);
                flushSeekChunks();
                if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
                finishSeek();
                // The next level tick runs before the next replay tick, so a paused landing is frozen from here.
                levelFreeze.freeze = mIsPaused && mExportTimelinePhase == ReplayExportTimelinePhase::Inactive;
            }
            return;
        }

        if (mExportTimelinePhase == ReplayExportTimelinePhase::Continuous) {
            if (mCurrentTick > mExportTargetTick) {
                throw std::runtime_error("Continuous export replay tick moved past its frame target");
            }
            if (mCurrentTick < mExportTargetTick && !advanceReplayTick(false)) {
                throw std::runtime_error("Replay ended before the continuous export frame target");
            }
            return;
        }

        bool const exportActive = mExportTimelinePhase != ReplayExportTimelinePhase::Inactive;
        if (mIsPaused) {
            levelFreeze.freeze = !exportActive;
            holdItemUseBetweenTicks();
            return;
        }
        mPlaybackTickAccumulator += mPlaybackSpeed;
        int const ticksToAdvance  = static_cast<int>(mPlaybackTickAccumulator);
        mPlaybackTickAccumulator -= static_cast<float>(ticksToAdvance);
        if (ticksToAdvance == 0) holdItemUseBetweenTicks();
        for (int tick = 0;
             tick < ticksToAdvance && !mChunkInjectionPending && !mPendingSnapshotApply && !mPendingReplayDimension;
             ++tick) {
            int const before = mCurrentTick;
            syncPreviewObserverForTick(before + 1);
            if (!advanceReplayTick(true)) break;
            if (mCurrentTick != before) markReplayTickAdvanced();
        }
        // The next client tick runs before its replay tick, so freeze it only when that replay tick will not advance.
        bool const settled = !mChunkInjectionPending && !mPendingSnapshotApply && !mPendingReplayDimension;
        levelFreeze.freeze = settled && !exportActive && mPlaybackTickAccumulator + mPlaybackSpeed < 1.0f;
    } catch (std::exception const& e) {
        getLogger().error("Replay session failed: {}", e.what());
        stop();
    }
}

void ReplaySession::updateControlPlane() {
    if (mStopRequested.exchange(false, std::memory_order_acq_rel)) {
        if (mActive) stop();
        return;
    }
    if (!mActive || !mReplayWorldJoined || !mPendingReplayDimension) return;

    try {
        auto request = mDimensionTransitionRequest;
        if (!request) throw std::runtime_error("Replay dimension transition lost its server request");

        if (mRequestedSeekTick.load(std::memory_order_acquire) >= 0) {
            auto expected = DimensionTransitionStatus::Pending;
            if (request->status.compare_exchange_strong(
                    expected,
                    DimensionTransitionStatus::Cancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                )) {
                mPendingSnapshotApply.reset();
                mPendingReplayDimension.reset();
                mDimensionTransitionRequest.reset();
                mDimensionTransitionStartedAt      = {};
                mDimensionTransitionSettledUpdates = 0;
                mSeekTargetTick                    = -1;
                mExportSeekRequested               = false;
                mSnapMovementDuringSeek            = false;
                if (!mChunkInjectionPending) resetDimensionScopedReplayState();
                (void)refreshReplayPlayer();
                return;
            }
        }

        processPendingDimensionTransition();
    } catch (std::exception const& e) {
        getLogger().error("Replay control plane failed: {}", e.what());
        mReplayFailed = true;
        stop();
    } catch (...) {
        getLogger().error("Replay control plane failed");
        mReplayFailed = true;
        stop();
    }
}

bool ReplaySession::init(std::filesystem::path filePath) {
    auto path       = filePath.string();
    int  errorCode  = 0;
    auto rawArchive = zip_open(path.c_str(), ZIP_RDONLY, &errorCode);
    if (!rawArchive) {
        getLogger().error("Unable to open replay archive: {}", filePath);
        return false;
    }
    std::unique_ptr<zip_t, decltype(&zip_close)> archive(rawArchive, &zip_close);

    auto metadata = readArchiveEntry(archive.get(), "metadata.json");
    if (!metadata) {
        getLogger().error("Replay archive does not contain metadata.json");
        return false;
    }

    mMeta = PlaybackMeta::fromJson(*metadata);
    if (mMeta.chunks.empty()) {
        getLogger().error("Replay archive does not contain replay chunks");
        return false;
    }
    mReaders.clear();
    mSnapshotContexts.clear();
    mDimensionTransitionTicks.clear();
    mChunkPackets.clear();
    mInlineLevelChunkPacketIndices.clear();
    mInlineSubChunkPacketIndices.clear();

    int64_t chunkStartTick = 0;
    for (auto const& [chunkName, chunkMeta] : mMeta.chunks) {
        auto chunk = readArchiveEntry(archive.get(), chunkName);
        if (!chunk) {
            getLogger().error("Replay archive does not contain {}", chunkName);
            return false;
        }
        auto reader           = std::make_unique<ReplayReader>(*chunk);
        auto context          = reader->readSnapshotContext();
        auto recordTransition = [&](int64_t transitionTick) {
            if (transitionTick >= 0 && transitionTick <= std::max(0, mMeta.totalTicks)
                && transitionTick <= std::numeric_limits<int>::max()) {
                mDimensionTransitionTicks.emplace_back(static_cast<int>(transitionTick));
            }
        };
        // A dimension change rotates the chunk instead of leaving a packet, so the contexts are the authority.
        if (!mSnapshotContexts.empty() && mSnapshotContexts.back().dimensionId != context.dimensionId) {
            recordTransition(chunkStartTick - 1);
        }
        for (int offset : reader->readDimensionTransitionTickOffsets()) {
            recordTransition(chunkStartTick + static_cast<int64_t>(offset));
        }
        mReaders.emplace_back(std::move(reader));
        mSnapshotContexts.emplace_back(context);
        chunkStartTick += std::max(0, chunkMeta.duration);
    }
    std::sort(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end());
    mDimensionTransitionTicks.erase(
        std::unique(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end()),
        mDimensionTransitionTicks.end()
    );
    for (int cacheIndex = 0;; ++cacheIndex) {
        auto entryName = "level_chunk_caches/" + std::to_string(cacheIndex) + ".bin";
        if (zip_name_locate(archive.get(), entryName.c_str(), 0) < 0) break;

        auto cache = readArchiveEntry(archive.get(), entryName);
        if (!cache || !appendChunkCache(*cache, mChunkPackets)) {
            getLogger().error("Unable to read replay chunk cache {}", entryName);
            return false;
        }
    }

    mReplayFilePath          = std::move(filePath);
    mCurrentTick             = 0;
    mReaderIndex             = 0;
    mSeekTargetTick          = -1;
    mExportSeekRequested     = false;
    mSnapMovementDuringSeek  = false;
    mExportTimelinePhase     = ReplayExportTimelinePhase::Inactive;
    mExportTargetTick        = -1;
    mPlaybackSpeed           = 1.0f;
    mPlaybackTickAccumulator = 0.0f;
    mReplayTime.reset();
    mPendingReplayDimension.reset();
    mPendingSnapshotApply.reset();
    mDimensionTransitionRequest.reset();
    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionGeneration     = 0;
    mCompletedDimensionGeneration      = 0;
    mDimensionTransitionStartedAt      = {};
    mRequestedSeekTick.store(-1, std::memory_order_relaxed);
    mReplayWorldJoined = false;
    mWorldReady        = false;
    mReplayFailed      = false;
    mIsPaused          = true;
    mStopRequested.store(false, std::memory_order_relaxed);
    mInjectingPacket.store(nullptr, std::memory_order_release);
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mReplayDimension.store(nullptr, std::memory_order_release);
    mInitialSnapshotApplied = false;
    mChunkInjectionPending  = false;
    clearSeekChunkDeferral();
    mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;
    restoreObserverSkin();
    mReplayPlayer = nullptr;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
        mRetainedReplayChunks.clear();
    }
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mPendingSnapshotLocalPlayer.reset();
    mPendingSnapshotGamePackets.clear();
    mAppliedConfigurationPackets.clear();
    mRecordedEntityIds.clear();
    mRecordedLocalPlayerId.reset();
    gReplayHandBobTick.clear();
    gReplayHotbars.clear();
    clearReplayPlayers(true);
    visuals::setReplaySubject(std::nullopt);
    mEntityRenderKeys.clear();
    visuals::clearReplayEntityPoses();
    mReplayObjectiveNames.clear();
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mApplyingChunkSnapshot      = false;
    mChunkInjectionPlanPrepared = false;
    mCenterChunksReady          = false;
    mChunkInjectionStartedAt    = {};
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
    clearNetworkContext();
    return true;
}

bool ReplaySession::prepareReplayResourcePacks(std::vector<PlaybackSerializedGamePacket> const& packets) {
    mReplayResourcePacksInfo.store(nullptr, std::memory_order_release);
    mReplayResourcePackStack.store(nullptr, std::memory_order_release);
    mReplayCachedResourcePacksLoaded = false;

    auto client = ll::service::getClientInstance();
    if (!client) return false;

    auto findPacket = [&packets](MinecraftPacketIds packetId) -> PlaybackSerializedGamePacket const* {
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            if (it->mPacketId == static_cast<int32_t>(packetId)) return &*it;
        }
        return nullptr;
    };
    auto decodePacket = [](PlaybackSerializedGamePacket const& serialized) {
        auto packet = MinecraftPackets::createPacket(static_cast<MinecraftPacketIds>(serialized.mPacketId));
        if (!packet) throw std::runtime_error("Unable to create a recorded resource-pack packet");

        ReadOnlyBinaryStream stream(serialized.mPayload, false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) {
            throw std::runtime_error(
                std::format("Unable to decode recorded resource-pack packet {}", serialized.mPacketId)
            );
        }
        return packet;
    };

    auto&       repository      = client->getResourcePackRepository();
    auto const* serializedInfo  = findPacket(MinecraftPacketIds::ResourcePacksInfo);
    auto const* serializedStack = findPacket(MinecraftPacketIds::ResourcePackStack);

    std::shared_ptr<ResourcePacksInfoPacket> info;
    if (serializedInfo) {
        info      = std::static_pointer_cast<ResourcePacksInfoPacket>(decodePacket(*serializedInfo));
        auto keys = info->toPacksInfoData().collectKeys();
        mReplayCachedResourcePacksLoaded = true;
        repository.addCachedResourcePacks(&keys);
        // Packs embedded in the replay take over from the local download cache; old replays keep using the cache.
        try {
            mMountedReplayPacks = io::packs::mountReplayPacks(repository, *info, mReplayFilePath);
        } catch (std::exception const& exception) {
            getLogger().warn("Unable to mount embedded resource packs, using the local cache: {}", exception.what());
        }
    }

    if (!serializedStack) return true;

    auto stack = std::static_pointer_cast<ResourcePackStackPacket>(decodePacket(*serializedStack));
    if (!info) return true;
    mReplayResourcePacksInfo.store(std::move(info), std::memory_order_release);
    mReplayResourcePackStack.store(std::move(stack), std::memory_order_release);
    return true;
}

void ReplaySession::prepareRecordedBlockRegistry(std::vector<PlaybackSerializedGamePacket> const& packets) {
    mReplayStartGame.reset();
    mRecordedBlockRegistryApplied = false;

    PlaybackSerializedGamePacket const* serialized = nullptr;
    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        if (it->mPacketId == static_cast<int32_t>(MinecraftPacketIds::StartGame)) {
            serialized = &*it;
            break;
        }
    }
    if (!serialized) {
        getLogger().debug("Replay contains no recorded StartGame packet; custom blocks stay unregistered");
        return;
    }

    try {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::StartGame);
        if (!packet) throw std::runtime_error("Unable to create a recorded StartGame packet");

        ReadOnlyBinaryStream stream(serialized->mPayload, false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) {
            throw std::runtime_error("Unable to decode the recorded StartGame packet");
        }

        auto startGame = std::static_pointer_cast<StartGamePacket>(packet);
        getLogger().debug("Replay carries {} recorded block properties", startGame->mBlockProperties->size());
        mReplayStartGame = std::move(startGame);
    } catch (std::exception const& exception) {
        getLogger().error("Unable to prepare the recorded block registry: {}", exception.what());
    } catch (...) {
        getLogger().error("Unable to prepare the recorded block registry");
    }
}

void ReplaySession::prepareRecordedItemRegistry(std::vector<PlaybackSerializedGamePacket> const& packets) {
    std::lock_guard lock(mReplayItemRegistryMutex);
    mReplayItemRegistryPayload.clear();
    mRecordedItemRegistryApplied.store(false, std::memory_order_release);

    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        if (it->mPacketId != static_cast<int32_t>(MinecraftPacketIds::ItemRegistryPacket)) continue;
        mReplayItemRegistryPayload = it->mPayload;
        return;
    }
    getLogger().warn("Replay has no item registry; custom items in this recording stay unresolved");
}

std::shared_ptr<ItemRegistryPacket const> ReplaySession::takeRecordedItemRegistry() {
    std::string payload;
    {
        std::lock_guard lock(mReplayItemRegistryMutex);
        payload.swap(mReplayItemRegistryPayload);
    }
    if (payload.empty()) return nullptr;

    auto restore = [&] {
        std::lock_guard lock(mReplayItemRegistryMutex);
        if (mReplayItemRegistryPayload.empty()) mReplayItemRegistryPayload = std::move(payload);
    };
    auto decode = [&]() -> std::shared_ptr<ItemRegistryPacket const> {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::ItemRegistryPacket);
        if (!packet) throw std::runtime_error("Unable to create a recorded item registry packet");

        ReadOnlyBinaryStream stream(payload, false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) {
            throw std::runtime_error("Unable to decode the recorded item registry");
        }
        mRecordedItemRegistryApplied.store(true, std::memory_order_release);
        return std::static_pointer_cast<ItemRegistryPacket const>(std::move(packet));
    };

    // ItemRegistryPacket::read locks the thread-local item registry and asserts when that lock fails. Replay start
    // is still the main menu, so the bytes stay stored until this hook. The join dispatcher has already installed
    // that registry; if it has not, keep the bytes for a later packet instead of crashing.
    try {
        if (ItemRegistryManager::getItemRegistry().mWeakRegistry.expired()) {
            auto client = ll::service::getClientInstance();
            if (!client) {
                restore();
                getLogger().error("Unable to decode the recorded item registry because the client is unavailable");
                return nullptr;
            }
            auto registry = client->getItemRegistry();
            if (registry.mWeakRegistry.expired()) {
                restore();
                getLogger().error(
                    "Unable to decode the recorded item registry because the client item registry is not ready"
                );
                return nullptr;
            }
            ItemRegistryManager::ScopedItemRegistry scope(registry.mWeakRegistry);
            return decode();
        }
        return decode();
    } catch (std::exception const& exception) {
        getLogger().error("Unable to prepare the recorded item registry: {}", exception.what());
    } catch (...) {
        getLogger().error("Unable to prepare the recorded item registry");
    }
    return nullptr;
}

void ReplaySession::logRecordedItemRegistryApplied(ItemRegistryPacket const& packet) const {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto const registry = client->getItemRegistry();
    // lookupByName locks the ref and asserts when it is empty.
    if (registry.mWeakRegistry.expired()) return;

    int custom   = 0;
    int resolved = 0;
    for (auto const& item : *packet.mItems) {
        auto const& name = item.mName->getString();
        if (name.starts_with("minecraft:")) continue;
        ++custom;
        if (registry.lookupByName(HashedString{name})) ++resolved;
    }
    getLogger()
        .info("Registered recorded item ids items={} custom={} resolved={}", packet.mItems->size(), custom, resolved);
}

void ReplaySession::applyRecordedBlockRegistry() {
    // Two hooks land here while the replay world loads: the StartGame handler and the LevelRenderer constructor
    // (the only caller of finalizeBlockComponentStorageForRendering). The bare flag below is a check-then-act race
    // that lets both run the registration concurrently, so serialize them and re-check under the lock.
    std::lock_guard lock(mRecordedBlockRegistryMutex);

    auto const threadId = std::hash<std::thread::id>{}(std::this_thread::get_id());
    if (mRecordedBlockRegistryApplied) {
        getLogger().debug("Recorded block registry already applied (t{})", threadId);
        return;
    }
    auto startGame = mReplayStartGame;
    if (!startGame) return;
    getLogger().debug("Applying recorded block registry (t{})", threadId);
    mRecordedBlockRegistryApplied = true;

    auto& properties = *startGame->mBlockProperties;
    if (properties.empty()) {
        getLogger().debug("Recorded StartGame carries no block properties");
        return;
    }

    try {
        auto level = ll::service::getLevel();
        if (!level) throw std::runtime_error("Level is unavailable");
        level->initializeBlockDefinitionGroup();

        auto* definitions = level->getBlockDefinitions();
        if (!definitions) throw std::runtime_error("Block definitions are unavailable");

        auto& registry = BlockTypeRegistry::get();

        // The registry exposes no updater version accessor, so read it back from a vanilla permutation.
        std::optional<uint> updaterVersion;
        for (auto& [name, blockType] : *BlockTypeRegistry::get().mBlockLookupMap) {
            if (updaterVersion) break;
            for (auto const& permutation : *blockType->mBlockPermutations) {
                if (!permutation) continue;
                auto const& serializationId = permutation->mSerializationId;
                if (!serializationId->contains("version", Tag::Type::Int)) continue;
                updaterVersion = static_cast<uint>(serializationId->at("version").get<IntTag>().data);
                break;
            }
        }
        if (!updaterVersion) throw std::runtime_error("Unable to resolve the block updater version");

        auto const preloadedGeometry = preloadRecordedBlockGeometry(properties);
        definitions->digestServerBlockProperties(properties);

        size_t                              registered = 0;
        std::vector<BlockDefinition const*> newlyRegisteredDefinitions;
        for (auto const& [name, tag] : properties) {
            // The vanilla blocks have already been registered locally; only custom blocks from recorded resource packs
            // need to be registered here.
            if (registry.lookupByName(HashedString{name}, false)) continue;

            auto const* definition = definitions->tryGetBlockDefinition(name);
            if (!definition) continue;

            auto blockType = definitions->registerDataDrivenBlock(definition->mDescription);
            if (!blockType) continue;

            blockType->createBlockPermutations(*updaterVersion);
            definitions->initBlockTypeFromDefinition(*blockType, *definition);
            newlyRegisteredDefinitions.push_back(definition);
            ++registered;
        }

        auto const injectedVisuals = injectRecordedBlockMaterialComponents(properties);

        // Component descriptions require mAllowModifyingComponents == true, so initialize newly registered blocks
        // (and never already-finalized vanilla/prior blocks) before finalizeBlockComponentStorage() locks them.
        for (auto const* definition : newlyRegisteredDefinitions) {
            definitions->initializeBlockFromDefinition(*definition, *level);
        }

        // 26.51 no longer exports BlockTypeRegistry::setupDirectAccessBlocks; it is inlined into
        // VanillaWorldSystems::init, which already ran when this replay world was created, and it is the only
        // writer of mDirectAccessBlocks, so there is nothing left to refresh here.
        registry.finalizeBlockComponentStorage();

        auto& palette = level->getBlockPalette();
        palette.initFromBlockDefinitions();
        palette.cacheBlockComponentData();
        for (auto const& [name, tag] : properties) {
            auto blockType = registry.lookupByName(HashedString{name}, false);
            if (!blockType) continue;
            for (auto const& permutation : *blockType->mBlockPermutations) {
                if (permutation) permutation->cacheComponentData();
            }
        }

        getLogger().debug(
            "Registered {} recorded custom block types (geometry={}, visuals={})",
            registered,
            preloadedGeometry,
            injectedVisuals
        );
    } catch (std::exception const& exception) {
        getLogger().error("Unable to register recorded custom blocks: {}", exception.what());
    } catch (...) {
        getLogger().error("Unable to register recorded custom blocks");
    }
}

size_t ReplaySession::preloadRecordedBlockGeometry(std::vector<ServerBlockProperty> const& properties) {
    auto client = ll::service::getClientInstance();
    if (!client) return 0;

    auto                            geometry         = client->getGeometryGroup();
    auto&                           resourceManager  = client->getResourcePackManager();
    auto                            minEngineVersion = MinEngineVersion::fromString("0.0.0");
    std::unordered_set<std::string> geometryPaths;

    std::function<void(CompoundTagVariant const&)> collectGeometryNames;
    collectGeometryNames = [&](CompoundTagVariant const& value) {
        if (auto const* stringTag = std::get_if<StringTag>(&value.mTagStorage)) {
            auto const& geometryName = static_cast<std::string const&>(*stringTag);
            auto const  marker       = geometryName.find(".block.");
            if (!geometryName.starts_with("geometry.") || marker == std::string::npos) return;

            auto path  = std::string{"models/blocks/"};
            path      += geometryName.substr(9, marker - 9);
            path      += '/';
            path      += geometryName.substr(marker + 7);
            for (auto& character : path) {
                if (character == '.') character = '_';
            }
            path += ".geometry.json";
            geometryPaths.insert(std::move(path));
            return;
        }
        if (auto const* compound = std::get_if<CompoundTag>(&value.mTagStorage)) {
            for (auto const& [key, nested] : *compound) collectGeometryNames(nested);
        }
    };

    for (auto const& [name, tag] : properties) {
        for (auto const& [key, value] : *tag) collectGeometryNames(value);
    }

    size_t loaded = 0;
    for (auto const& path : geometryPaths) {
        std::string content;
        if (!resourceManager.load(ResourceLocation{Core::PathView{path}}, content)) continue;
        geometry->loadModelPackFromStringSync(path, content, minEngineVersion);
        ++loaded;
    }
    return loaded;
}

size_t ReplaySession::injectRecordedBlockMaterialComponents(std::vector<ServerBlockProperty> const& properties) {
    auto level = ll::service::getLevel();
    if (!level) return 0;

    auto* definitions = level->getBlockDefinitions();
    if (!definitions) return 0;

    auto& registry = BlockTypeRegistry::get();

    // Blocks without material_instances fall back to the legacy blocks.json texture aliases.
    std::unordered_map<std::string, std::string> legacyTextures;
    if (auto client = ll::service::getClientInstance()) {
        auto collectLegacyTextures = [&](std::string const& content) {
            auto root = nlohmann::ordered_json::parse(content, nullptr, false);
            if (root.is_object()) {
                for (auto const& [blockName, value] : root.items()) {
                    if (!value.is_object()) continue;
                    auto const textures = value.find("textures");
                    if (textures == value.end()) continue;

                    std::string texture;
                    if (textures->is_string()) {
                        texture = textures->get<std::string>();
                    } else if (textures->is_object()) {
                        for (auto const& face : {"north", "up", "south", "east", "west", "down"}) {
                            auto const faceTexture = textures->find(face);
                            if (faceTexture != textures->end() && faceTexture->is_string()) {
                                texture = faceTexture->get<std::string>();
                                break;
                            }
                        }
                    }
                    if (!texture.empty()) legacyTextures.emplace(blockName, std::move(texture));
                }
            }
        };
        client->getResourcePackManager().iteratePacks([&](PackInstance const& pack) {
            std::string content;
            if (pack.getResource(Core::Path{"blocks.json"}, content)) collectLegacyTextures(content);
        });
    }

    size_t modernInjected = 0;
    size_t legacyInjected = 0;
    for (auto const& [name, tag] : properties) {
        auto const* definition = definitions->tryGetBlockDefinition(name);
        if (!definition) continue;

        auto const* materialDescription =
            definition->mBaseComponents->getComponentDescription("minecraft:material_instances");
        auto const* geometryDescription = definition->mBaseComponents->getComponentDescription("minecraft:geometry");

        auto blockType = registry.lookupByName(HashedString{name}, false);
        if (!blockType) continue;
        auto& type = const_cast<BlockType&>(*blockType);

        // Init layer only: the engine's rendering pass skips blocks whose Rendering layer is already marked.
        auto inject = [&type](BlockComponentDescription const& description) {
            // 26.51 folds BlockGeometryDescription's storage overload into a stub; the context overload does the work
            // and the base class forwards it to the storage overload for other descriptions.
            auto initialize = [&](BlockComponentStorage& storage) {
                struct {
                    BlockComponentStorage* storage;
                    BlockType*             blockType;
                } layout{&storage, &type};
                static_assert(sizeof(layout) == sizeof(BlockComponentDescription::InitializationContext));
                description.initializeComponent(
                    *reinterpret_cast<BlockComponentDescription::InitializationContext*>(&layout)
                );
            };

            type.mComponents->allowComponentReplacement();
            initialize(*type.mComponents);
            BlockTypeComponentStorageFinalizer{}.finalizeComponentData(type);

            for (auto const& permutation : *type.mBlockPermutations) {
                if (!permutation) continue;
                permutation->mComponents->allowComponentReplacement();
                initialize(*permutation->mComponents);
                BlockComponentStorageFinalizer{}.finalizeComponentData(*permutation);
            }
        };

        if (materialDescription) {
            if (geometryDescription) {
                // The 26.51.6 headers stub NetworkedBlockComponentDescription, hiding the base; the call is virtual.
                inject(*geometryDescription);
            }
            inject(*reinterpret_cast<BlockMaterialInstancesDescription const*>(materialDescription));
            ++modernInjected;
            continue;
        }

        auto const legacy = legacyTextures.find(name);
        if (legacy == legacyTextures.end()) continue;

        BlockMaterialInstancesDescription const material{
            legacy->second,
            BlockRenderLayer::RenderlayerOpaque,
            1.0f,
            true,
            TintMethod::None,
            false,
            false,
            false,
            false
        };
        inject(material);
        ++legacyInjected;
    }
    getLogger().debug("Injected recorded block visuals: material={} legacy={}", modernInjected, legacyInjected);
    return modernInjected + legacyInjected;
}

void ReplaySession::releaseReplayResourcePacks() {
    mReplayResourcePacksInfo.store(nullptr, std::memory_order_release);
    mReplayResourcePackStack.store(nullptr, std::memory_order_release);
    if (!mReplayCachedResourcePacksLoaded) return;
    mReplayCachedResourcePacksLoaded = false;

    if (auto client = ll::service::getClientInstance()) {
        try {
            client->getResourcePackRepository().removePacksLoadedFromCache();
        } catch (std::exception const& exception) {
            getLogger().error("Unable to release replay resource packs loaded from cache: {}", exception.what());
        } catch (...) {
            getLogger().error("Unable to release replay resource packs loaded from cache");
        }
    }
    // The cache sources pointing at the mount are gone now, so its files can be deleted.
    if (mMountedReplayPacks) {
        io::packs::removeMountedPacks(*mMountedReplayPacks);
        mMountedReplayPacks.reset();
    }
}

void ReplaySession::onWorldReady() {
    if (!mInitialSnapshotApplied) {
        applyInitialSnapshot();
        mInitialSnapshotApplied = true;
        if (mReplayFailed) throw std::runtime_error("Unable to apply replay snapshot");
        return;
    }

    if (mChunkInjectionPending) {
        if (!tryFinishChunkInjection()) {
            if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
            return;
        }
    }
    if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");

    mWorldReady = true;
    getLogger().info("Replay ready at tick {} ({})", mCurrentTick, mIsPaused ? "paused" : "playing");
}

void ReplaySession::applyInitialSnapshot() {
    if (mReaders.empty()) throw std::runtime_error("Replay contains no chunks");

    applySnapshot(*mReaders.front(), true, true);
}

void ReplaySession::applySnapshot(ReplayReader& reader, bool followRecordedPlayer, bool serverPlayerRelocated) {
    if (mChunkInjectionPending) throw std::runtime_error("Previous replay snapshot is still being applied");

    if (!refreshReplayPlayer()) throw std::runtime_error("Replay player is unavailable while applying snapshot");
    auto* replayPlayer = mReplayPlayer;

    if (mReaderIndex >= mSnapshotContexts.size()) {
        throw std::runtime_error("Replay snapshot context index is out of range");
    }
    auto const&        snapshotContext = mSnapshotContexts[mReaderIndex];
    PlaybackView const snapshotView{
        snapshotContext.x,
        snapshotContext.y,
        snapshotContext.z,
        snapshotContext.yaw,
        snapshotContext.pitch
    };
    PlaybackView transitionView = snapshotView;
    if (!followRecordedPlayer) {
        auto const& position = replayPlayer->getPosition();
        auto const& rotation = replayPlayer->getRotation();
        transitionView       = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    }
    // Actor::teleport lands one eye height above the request. Snapshot and observer positions are already eye height.
    transitionView.y                   -= kServerEyeHeight;
    bool const relocateWithinDimension  = followRecordedPlayer && !serverPlayerRelocated;
    if (!ensureReplayDimension(DimensionType{snapshotContext.dimensionId}, transitionView, relocateWithinDimension)) {
        if (!mReplayFailed) {
            auto const generation = mDimensionTransitionRequest ? mDimensionTransitionRequest->generation : 0;
            mPendingSnapshotApply = PendingSnapshotApply{
                mReaderIndex,
                followRecordedPlayer,
                serverPlayerRelocated || followRecordedPlayer,
                generation
            };
        }
        return;
    }

    // The snapshot resends every column it covers, which supersedes chunks a seek still holds.
    clearSeekChunkDeferral();
    mPendingSnapshotLocalPlayer.reset();
    mPendingSnapshotGamePackets.clear();
    if (!clearRecordedEntities()) {
        mReplayFailed = true;
        return;
    }
    if (!clearReplayObjectives()) {
        mReplayFailed = true;
        return;
    }

    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
    }
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;
    mChunkInjectionPending   = false;
    mApplyingChunkSnapshot   = true;
    mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mChunkInjectionPlanPrepared = false;
    mCenterChunksReady          = false;
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
    mApplyingSnapshotChunks.clear();
    mPendingSnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mInjectedLevelChunks     = 0;
    mInjectedSubChunkPackets = 0;
    mInjectedSubChunkEntries = 0;
    mReusedSnapshotColumns   = 0;
    mDirectLevelChunks       = 0;
    mDirectSubChunkPackets   = 0;
    mDirectSubChunkEntries   = 0;

    reader.handleSnapshot(*this);
    reader.resetToStart();
    if (mReplayFailed) {
        mApplyingChunkSnapshot = false;
        return;
    }
    if (!mPendingSnapshotLocalPlayer) {
        getLogger().error("Replay snapshot {} is missing its CreateLocalPlayer action", mReaderIndex);
        mReplayFailed          = true;
        mApplyingChunkSnapshot = false;
        return;
    }
    std::optional<visuals::EntityRenderPose> snapshotCameraView;
    if (snapshotContext.hasView) {
        snapshotCameraView = visuals::EntityRenderPose{
            {snapshotContext.viewX, snapshotContext.viewY, snapshotContext.viewZ},
            snapshotContext.viewPitch,
            snapshotContext.viewYaw,
            snapshotContext.viewYaw,
            snapshotContext.viewYaw
        };
    }
    visuals::queueReplaySubjectCameraOffset(
        {snapshotContext.cameraOffsetX, snapshotContext.cameraOffsetY, snapshotContext.cameraOffsetZ},
        snapshotContext.fov,
        snapshotCameraView
    );

    PlaybackView view;
    if (followRecordedPlayer) {
        view = snapshotView;
        replayPlayer->moveTo(Vec3{view.x, view.y, view.z}, Vec2{view.pitch, view.yaw});
    } else if (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive && mExportCameraViewpoint) {
        auto const& position = *mExportCameraViewpoint;
        view                 = PlaybackView{position.x, position.y, position.z, position.yaw, position.pitch};
        replayPlayer->moveTo(Vec3{view.x, view.y, view.z}, Vec2{view.pitch, view.yaw});
    } else {
        auto const& position = replayPlayer->getPosition();
        auto const& rotation = replayPlayer->getRotation();
        view                 = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    }
    mChunkInjectionStartedAt = std::chrono::steady_clock::now();
    if (!prepareChunkInjectionPlan(view)) {
        mReplayFailed          = true;
        mApplyingChunkSnapshot = false;
        return;
    }
    mChunkPlanPreparationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt).count();
    getLogger().debug(
        "Prepared replay snapshot {} around ({:.3f}, {:.3f}, {:.3f}) before chunk injection (centerSource={})",
        mReaderIndex,
        view.x,
        view.y,
        view.z,
        followRecordedPlayer ? "recorded-player"
                             : (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive && mExportCameraViewpoint
                                    ? "export-camera"
                                    : "operator-player")
    );

    mChunkInjectionPending = true;
    getLogger().debug(
        "Starting distance-prioritized replay chunk streaming with {} columns, {} SubChunk packets, and at most {} "
        "LevelChunks in flight (plan {:.3f} ms)",
        mPendingLevelChunkIndices.size(),
        mPendingSubChunkPackets.size(),
        MAX_LEVEL_CHUNKS_IN_FLIGHT,
        mChunkPlanPreparationMs
    );
}

bool ReplaySession::ensureReplayDimension(
    DimensionType       target,
    PlaybackView const& view,
    bool                relocateWithinDimension
) {
    if (!mReplayPlayer) return false;
    bool const changesDimension = mReplayPlayer->getDimensionId() != target;
    if (!changesDimension && !relocateWithinDimension) {
        mReplayDimension.store(&mReplayPlayer->getDimension(), std::memory_order_release);
        return true;
    }
    if (mPendingReplayDimension) {
        if (*mPendingReplayDimension != target) {
            getLogger().error(
                "Replay requested dimension {} while dimension {} is still pending",
                target.mValue,
                mPendingReplayDimension->mValue
            );
            mReplayFailed = true;
        }
        return false;
    }
    if (changesDimension && mExportTimelinePhase != ReplayExportTimelinePhase::Inactive) {
        mExportCameraViewpoint.reset();
    }

    auto       request         = std::make_shared<DimensionTransitionRequest>();
    auto       playerUuid      = mReplayPlayer->getUuid();
    auto       replayLevelId   = mReplayLevelId;
    auto       sourceDimension = mReplayPlayer->getDimensionId();
    auto       position        = Vec3{view.x, view.y, view.z};
    auto       rotation        = Vec2{view.pitch, view.yaw};
    auto const generation      = mDimensionTransitionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    request->generation        = generation;
    auto* generationCounter    = &mDimensionTransitionGeneration;
    mObserverServerSyncEpoch.fetch_add(1, std::memory_order_acq_rel);
    mObserverServerPositionDirty = false;
    mLastObserverServerSyncChunk.reset();
    mObserverPreviewInRange = false;

    if (!clearRecordedEntities()) {
        getLogger().error(
            "Unable to clear recorded entities before leaving replay dimension {}",
            sourceDimension.mValue
        );
        mReplayFailed = true;
        return false;
    }

    // Stop applying source-dimension protection before destination packets can arrive.
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mReplayDimension.store(nullptr, std::memory_order_release);

    mPendingReplayDimension            = target;
    mDimensionTransitionRequest        = request;
    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionStartedAt      = std::chrono::steady_clock::now();
    getLogger().debug(
        "Replay dimension transition generation {} started at tick {} from dimension {} to {}",
        generation,
        mCurrentTick,
        sourceDimension.mValue,
        target.mValue
    );
    std::string targetName;
    if (auto const profile = mReplayDimensionProfile.load(std::memory_order_acquire)) {
        if (auto const it = profile->names.find(target.mValue); it != profile->names.end()) targetName = it->second;
    }

    ll::thread::ServerThreadExecutor::getDefault().execute([request,
                                                            generation,
                                                            generationCounter,
                                                            playerUuid,
                                                            replayLevelId = std::move(replayLevelId),
                                                            position,
                                                            rotation,
                                                            target,
                                                            targetName = std::move(targetName)] {
        if (generationCounter->load(std::memory_order_acquire) != generation) return;
        auto expected = DimensionTransitionStatus::Pending;
        if (!request->status.compare_exchange_strong(
                expected,
                DimensionTransitionStatus::Dispatching,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return;
        }

        auto fail = [&request] {
            auto expectedStatus = DimensionTransitionStatus::Dispatching;
            request->status.compare_exchange_strong(
                expectedStatus,
                DimensionTransitionStatus::Failed,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        };

        try {
            if (generationCounter->load(std::memory_order_acquire) != generation) return;
            auto level = ll::service::getLevel();
            if (!level || level->getLevelId() != replayLevelId) {
                getLogger().error("Unable to change replay dimension: replay server level is unavailable");
                fail();
                return;
            }

            auto targetDimension = level->getOrCreateDimension(target).lock();
            // Custom dimension ids are absent from VanillaDimensions, so resolve them by name instead.
            if (!targetDimension && !targetName.empty()) {
                targetDimension = level->getDimensionManager().getOrCreateDimension(targetName).lock();
            }
            auto* player = level->getPlayer(playerUuid);
            if (!targetDimension || !player) {
                getLogger().error(
                    "Unable to change replay dimension to {}: target dimension or server player is unavailable",
                    target.mValue
                );
                fail();
                return;
            }

            if (generationCounter->load(std::memory_order_acquire) != generation) {
                fail();
                return;
            }

            // Revisited dimensions need a fresh native LevelChunk publication.
            auto const& chunkPublisher = player->mChunkPublisherView.get();
            if (chunkPublisher) chunkPublisher->clearRegion();
            player->teleport(position, target, rotation);
            if (generationCounter->load(std::memory_order_acquire) != generation) {
                fail();
                return;
            }
            auto expectedStatus = DimensionTransitionStatus::Dispatching;
            if (!request->status.compare_exchange_strong(
                    expectedStatus,
                    DimensionTransitionStatus::Succeeded,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                )) {
                return;
            }
        } catch (std::exception const& e) {
            getLogger().error("Unable to change replay dimension to {}: {}", target.mValue, e.what());
            fail();
        } catch (...) {
            getLogger().error("Unable to change replay dimension to {}", target.mValue);
            fail();
        }
    });
    return false;
}

void ReplaySession::processPendingDimensionTransition() {
    if (!mPendingReplayDimension) return;
    if (!mDimensionTransitionRequest) {
        mReplayFailed = true;
        throw std::runtime_error("Replay dimension transition lost its server request");
    }

    auto const status = mDimensionTransitionRequest->status.load(std::memory_order_acquire);
    if (status == DimensionTransitionStatus::Failed || status == DimensionTransitionStatus::Cancelled) {
        mReplayFailed = true;
        throw std::runtime_error("Replay server dimension transition failed");
    }

    auto const elapsed          = std::chrono::steady_clock::now() - mDimensionTransitionStartedAt;
    bool const playerAvailable  = refreshReplayPlayer();
    bool const dimensionMatches = playerAvailable && mReplayPlayer->getDimensionId() == *mPendingReplayDimension;
    auto const waitComponent =
        playerAvailable ? mReplayPlayer->getEntityContext().tryGetComponent<LocalPlayerDimensionWaitComponent>()
                        : nullptr;
    bool const waitingForAcknowledgment =
        waitComponent && waitComponent->mWaitingForServerDimensionChangeAcknowledgment;
    auto const client               = ll::service::getClientInstance();
    bool const loadingScreenVisible = client
                                   && (client->isShowingLoadingScreen() || client->isShowingProgressScreen()
                                       || client->isShowingWorldProgressScreen());
    bool const readyToRender        = client && client->isReadyToRender();

    if (elapsed >= DIMENSION_TRANSITION_TIMEOUT) {
        getLogger().error(
            "Replay dimension transition generation {} to {} timed out after {} ms with request status {}; "
            "playerAvailable={}, currentDimension={}, dimensionMatches={}, waitingForAck={}, loadingScreen={}, "
            "readyToRender={}",
            mDimensionTransitionRequest->generation,
            mPendingReplayDimension->mValue,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            static_cast<int>(status),
            playerAvailable,
            playerAvailable ? mReplayPlayer->getDimensionId().mValue : 0,
            dimensionMatches,
            waitingForAcknowledgment,
            loadingScreenVisible,
            readyToRender
        );
        mReplayFailed = true;
        throw std::runtime_error("Replay dimension transition timed out");
    }

    if (!playerAvailable) {
        mDimensionTransitionSettledUpdates = 0;
        return;
    }

    if (status == DimensionTransitionStatus::Succeeded && dimensionMatches && waitingForAcknowledgment
        && elapsed >= DIMENSION_ACK_FALLBACK_DELAY) {
        if (client) {
            bool expected = false;
            if (mDimensionTransitionRequest->acknowledgmentFallbackQueued
                    .compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                PlayerActionPacket acknowledgment{};
                acknowledgment.mAction    = PlayerActionType::ChangeDimensionAck;
                acknowledgment.mRuntimeId = mReplayPlayer->getRuntimeID();
                client->getPacketSender().sendToServer(acknowledgment);
                getLogger().debug(
                    "Sent replay dimension acknowledgment fallback from the client for generation {}",
                    mDimensionTransitionRequest->generation
                );
            }
        }
    }

    bool const acknowledgmentGraceComplete =
        !waitingForAcknowledgment
        || mDimensionTransitionRequest->acknowledgmentFallbackQueued.load(std::memory_order_acquire);
    if (status != DimensionTransitionStatus::Succeeded || !dimensionMatches || !acknowledgmentGraceComplete) {
        mDimensionTransitionSettledUpdates = 0;
        return;
    }

    if (waitingForAcknowledgment && mDimensionTransitionSettledUpdates == 0) {
        getLogger().warn(
            "Replay dimension transition generation {} reached dimension {} but its client acknowledgment remains "
            "pending; continuing after the fallback grace period",
            mDimensionTransitionRequest->generation,
            mPendingReplayDimension->mValue
        );
    }

    if (!client) {
        mDimensionTransitionSettledUpdates = 0;
        return;
    }

    if (loadingScreenVisible && mDimensionTransitionSettledUpdates == 0) {
        getLogger().debug(
            "Replay dimension transition generation {} reached dimension {} behind the loading screen "
            "(readyToRender={})",
            mDimensionTransitionRequest->generation,
            mPendingReplayDimension->mValue,
            readyToRender
        );
    }

    if (++mDimensionTransitionSettledUpdates >= DIMENSION_TRANSITION_SETTLE_UPDATES) {
        completeReplayDimensionTransition();
    }
}

void ReplaySession::completeReplayDimensionTransition() {
    if (!refreshReplayPlayer()) return;
    auto const completedGeneration = mDimensionTransitionRequest->generation;
    auto const elapsed             = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - mDimensionTransitionStartedAt
    );
    getLogger().debug(
        "Replay dimension transition generation {} completed at tick {} in dimension {} after {} ms",
        completedGeneration,
        mCurrentTick,
        mReplayPlayer->getDimensionId().mValue,
        elapsed.count()
    );
    mReplayDimension.store(&mReplayPlayer->getDimension(), std::memory_order_release);
    mPendingReplayDimension.reset();
    mDimensionTransitionRequest.reset();
    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionStartedAt      = {};
    mCompletedDimensionGeneration      = completedGeneration;
    resetDimensionScopedReplayState();
    // The observer keeps its old coordinates in the new dimension; bring it to the recorded player once that is known.
    if (mExportTimelinePhase == ReplayExportTimelinePhase::Inactive) mTeleportToSubjectPending = true;
}

void ReplaySession::resetDimensionScopedReplayState() {
    clearSeekChunkDeferral();
    mChunkInjectionPending      = false;
    mChunkInjectionPlanPrepared = false;
    mApplyingChunkSnapshot      = false;
    mCenterChunksReady          = false;
    mSnapshotGamePacketPhase    = SnapshotGamePacketPhase::StreamingChunks;
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mPendingSnapshotLocalPlayer.reset();
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mCenterChunkPositions.clear();
    mRecordedEntityIds.clear();
    gReplayHandBobTick.clear();
    gReplayHotbars.clear();
    clearReplayPlayers(false);
    mRecordedLocalPlayerId.reset();
    visuals::setReplaySubject(std::nullopt);
    mEntityRenderKeys.clear();
    visuals::clearReplayEntityPoses();
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
        mRetainedReplayChunks.clear();
    }
}

bool ReplaySession::refreshReplayPlayer() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player || !isReplayLevel(player->getLevel())) {
        // Stale on failure: callers must not act on a player object the engine may have already destroyed.
        mReplayPlayer = nullptr;
        return false;
    }

    mReplayPlayer = player;
    if (mPendingReplayDimension && player->getDimensionId() != *mPendingReplayDimension) {
        mReplayDimension.store(nullptr, std::memory_order_release);
    } else {
        mReplayDimension.store(&player->getDimension(), std::memory_order_release);
    }
    return true;
}

static void noteSkippedReplayChunk(bool levelChunk, int packetDimension, int currentDimension, int tick) {
    struct Signature {
        bool levelChunk;
        int  packetDimension;
        int  currentDimension;
        int  tick;
    };
    static Signature last{};
    static int       suppressed = 0;
    static bool      haveLast   = false;
    Signature const  now{levelChunk, packetDimension, currentDimension, tick};
    if (haveLast && now.levelChunk == last.levelChunk && now.packetDimension == last.packetDimension
        && now.currentDimension == last.currentDimension && now.tick == last.tick) {
        ++suppressed;
        return;
    }
    int const repeatedBefore = haveLast ? suppressed : 0;
    getLogger().debug(
        "replay chunk skipped kind={} dimension={} current={} tick={} repeatedBefore={}",
        levelChunk ? "level" : "sub",
        packetDimension,
        currentDimension,
        tick,
        repeatedBefore
    );
    last       = now;
    suppressed = 0;
    haveLast   = true;
}

std::optional<ReplaySession::FollowedChunkCenter>
ReplaySession::followedPlayerChunkCenter(DimensionType const& snapshotDimension) {
    if (!visuals::isReplaySubjectViewDriven() || visuals::isReplaySubjectRecorder()) return std::nullopt;
    std::string const choice = replaySubjectChoice();
    if (choice.empty() || !mReplayPlayer) return std::nullopt;

    ActorUniqueID id;
    if (auto const raw = parseEntityChoice(choice)) {
        id = ActorUniqueID{*raw};
    } else {
        std::lock_guard lock(mReplayPlayersMutex);
        auto const      it = mReplayPlayers.find(choice);
        if (it == mReplayPlayers.end() || it->second.recorder || !it->second.present) return std::nullopt;
        id = it->second.id;
    }

    Actor* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
    if (!actor || actor == mReplayPlayer || actor->getDimensionId() != snapshotDimension) return std::nullopt;
    auto const position = actor->getPosition();
    if (!std::isfinite(position.x) || !std::isfinite(position.z)) return std::nullopt;
    return FollowedChunkCenter{choice, position.x, position.z};
}

bool ReplaySession::prepareChunkInjectionPlan(PlaybackView const& view) {
    struct PrioritizedLevelChunk {
        ChunkPos pos;
        int64_t  distanceSquared;
        int      index;
    };
    struct PrioritizedSubChunk {
        PendingSubChunkPacket packet;
        int64_t               distanceSquared;
    };

    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) {
        getLogger().error("Replay dimension disappeared while preparing chunk streaming");
        return false;
    }

    int const   snapshotChunkX = static_cast<int>(std::floor(view.x / 16.0f));
    int const   snapshotChunkZ = static_cast<int>(std::floor(view.z / 16.0f));
    int         centerX        = snapshotChunkX;
    int         centerZ        = snapshotChunkZ;
    bool        followCenter   = false;
    std::string followSubject;
    if (auto const followed = followedPlayerChunkCenter(replayDimension->getDimensionId())) {
        centerX       = static_cast<int>(std::floor(followed->x / 16.0f));
        centerZ       = static_cast<int>(std::floor(followed->z / 16.0f));
        followCenter  = true;
        followSubject = followed->subject;
    }
    auto const distanceSquared = [centerX, centerZ](ChunkPos const& pos) {
        int64_t const dx = static_cast<int64_t>(pos.x) - centerX;
        int64_t const dz = static_cast<int64_t>(pos.z) - centerZ;
        return dx * dx + dz * dz;
    };

    std::vector<PrioritizedLevelChunk>                    levelChunks;
    std::unordered_set<ChunkPos>                          levelChunkPositions;
    std::unordered_set<ChunkPos>                          requestModeLevelChunks;
    std::unordered_map<ChunkPos, SnapshotColumnIdentity>  targetColumns;
    std::unordered_map<ChunkPos, std::unordered_set<int>> subChunkIndicesByColumn;
    size_t                                                skippedBlobCachePackets = 0;
    size_t                                                skippedDimensionPackets = 0;
    levelChunks.reserve(mPendingLevelChunkIndices.size());
    levelChunkPositions.reserve(mPendingLevelChunkIndices.size());
    requestModeLevelChunks.reserve(mPendingLevelChunkIndices.size());

    for (int index : mPendingLevelChunkIndices) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
        if (!packet) {
            getLogger().error("Unable to create a LevelChunk packet while preparing replay streaming");
            return false;
        }

        ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(index)], false);
        if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) {
            getLogger().error("Unable to decode replay LevelChunk packet {} while preparing streaming", index);
            return false;
        }

        auto const& levelChunk = static_cast<LevelChunkPacket const&>(*packet);
        if (static_cast<bool>(levelChunk.mCacheEnabled)) {
            ++skippedBlobCachePackets;
            continue;
        }
        auto const packetDimension = static_cast<DimensionType const&>(levelChunk.mDimensionId);
        if (packetDimension != replayDimension->getDimensionId()) {
            ++skippedDimensionPackets;
            noteSkippedReplayChunk(
                true,
                packetDimension.mValue,
                replayDimension->getDimensionId().mValue,
                mCurrentTick
            );
            continue;
        }

        ChunkPos const pos = *levelChunk.mPos;
        if (!levelChunkPositions.emplace(pos).second) {
            auto const duplicate = std::find_if(levelChunks.begin(), levelChunks.end(), [&pos](auto const& existing) {
                return existing.pos == pos;
            });
            if (duplicate != levelChunks.end()) duplicate->index = index;
            requestModeLevelChunks.emplace(pos);
            targetColumns[pos].levelChunkIndex = index;
            continue;
        }
        requestModeLevelChunks.emplace(pos);
        targetColumns[pos].levelChunkIndex = index;
        levelChunks.push_back(PrioritizedLevelChunk{pos, distanceSquared(pos), index});
    }

    std::stable_sort(levelChunks.begin(), levelChunks.end(), [](auto const& left, auto const& right) {
        if (left.distanceSquared != right.distanceSquared) return left.distanceSquared < right.distanceSquared;
        if (left.pos.x != right.pos.x) return left.pos.x < right.pos.x;
        return left.pos.z < right.pos.z;
    });

    mCenterChunkPositions.clear();
    for (auto const& levelChunk : levelChunks) {
        if (std::abs(levelChunk.pos.x - centerX) <= 2 && std::abs(levelChunk.pos.z - centerZ) <= 2) {
            mCenterChunkPositions.emplace(levelChunk.pos);
        }
    }
    if (mCenterChunkPositions.empty() && !levelChunks.empty()) {
        mCenterChunkPositions.emplace(levelChunks.front().pos);
    }

    auto& protectedChunks = mApplyingChunkSnapshot ? mApplyingSnapshotChunks : mSnapshotChunks;
    protectedChunks.insert(levelChunkPositions.begin(), levelChunkPositions.end());

    std::vector<PrioritizedSubChunk> subChunks;
    subChunks.reserve(mPendingSubChunkIndices.size());
    mRemainingSubChunkPacketsByColumn.clear();
    for (int index : mPendingSubChunkIndices) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
        if (!packet) {
            getLogger().error("Unable to create a SubChunk packet while preparing replay streaming");
            return false;
        }

        ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(index)], false);
        if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) {
            getLogger().error("Unable to decode replay SubChunk packet {} while preparing streaming", index);
            return false;
        }

        auto const& subChunk = static_cast<SubChunkPacket const&>(*packet);
        if (static_cast<bool>(subChunk.mCacheEnabled)) {
            ++skippedBlobCachePackets;
            continue;
        }
        auto const packetDimension = static_cast<DimensionType const&>(subChunk.mDimensionType);
        if (packetDimension != replayDimension->getDimensionId()) {
            ++skippedDimensionPackets;
            noteSkippedReplayChunk(
                false,
                packetDimension.mValue,
                replayDimension->getDimensionId().mValue,
                mCurrentTick
            );
            continue;
        }

        auto const& entries = *subChunk.mSubChunkData;
        if (entries.empty()) {
            getLogger().error("Replay SubChunk packet {} contains no successful entries", index);
            return false;
        }
        if (entries.size() > MAX_SUB_CHUNK_ENTRIES_PER_PACKET) {
            getLogger().error(
                "Replay SubChunk packet {} has {} entries, exceeding the per-packet limit {}",
                index,
                entries.size(),
                MAX_SUB_CHUNK_ENTRIES_PER_PACKET
            );
            return false;
        }

        PendingSubChunkPacket pending;
        pending.index                                          = index;
        auto const&                                     center = *subChunk.mCenterPos;
        std::vector<SubChunkPacket::SubChunkPacketData> playableEntries;
        playableEntries.reserve(entries.size());
        for (auto const& entry : entries) {
            auto const result = static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult);
            if (!isSuccessfulSubChunkResult(result)) continue;
            auto const&    offset = *entry.mSubChunkPosOffset;
            ChunkPos const target{center.x + static_cast<int>(offset.mX), center.z + static_cast<int>(offset.mZ)};
            if (!levelChunkPositions.contains(target) && !mSnapshotChunks.contains(target)) {
                if (mApplyingChunkSnapshot) {
                    getLogger().error(
                        "Replay snapshot SubChunk packet {} targets column ({}, {}) without a LevelChunk",
                        index,
                        target.x,
                        target.z
                    );
                    return false;
                }
                continue;
            }

            playableEntries.emplace_back(entry);
            subChunkIndicesByColumn[target].emplace(center.y + static_cast<int>(offset.mY));
            if (std::find(pending.targets.begin(), pending.targets.end(), target) == pending.targets.end()) {
                pending.targets.emplace_back(target);
            }
        }
        if (pending.targets.empty()) continue;
        if (playableEntries.size() != entries.size()) {
            auto filteredPacket           = subChunk;
            *filteredPacket.mSubChunkData = std::move(playableEntries);
            PlaybackBuffer filteredPayload;
            filteredPacket.write(filteredPayload);
            pending.payload = std::move(filteredPayload.mBuffer);
        }
        int64_t priority = std::numeric_limits<int64_t>::max();
        for (auto const& target : pending.targets) {
            targetColumns[target].subChunkIndices.emplace_back(index);
            priority = std::min(priority, distanceSquared(target));
        }
        subChunks.push_back(PrioritizedSubChunk{std::move(pending), priority});
    }

    for (auto& [_, identity] : targetColumns) {
        std::sort(identity.subChunkIndices.begin(), identity.subChunkIndices.end());
        identity.subChunkIndices.erase(
            std::unique(identity.subChunkIndices.begin(), identity.subChunkIndices.end()),
            identity.subChunkIndices.end()
        );
    }

    mReusableSnapshotColumns.clear();
    for (auto const& [pos, identity] : targetColumns) {
        auto applied = mAppliedSnapshotColumns.find(pos);
        if (identity.levelChunkIndex >= 0 && applied != mAppliedSnapshotColumns.end() && applied->second == identity
            && !mDirtySnapshotColumns.contains(pos)) {
            mReusableSnapshotColumns.emplace(pos);
        }
    }
    mReusedSnapshotColumns = mReusableSnapshotColumns.size();

    mChunkIsolationDimension = replayDimension->getDimensionId();

    int const    minimumSubChunk = static_cast<int>(replayDimension->mHeightRange->mMin) / 16;
    size_t const subChunkCount   = static_cast<size_t>(replayDimension->getHeightInSubchunks());
    if (subChunkCount == 0) {
        getLogger().error("Replay dimension has no subchunk slots");
        return false;
    }

    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    bool const seekFlushedBatch = mSeekFlushedBatch;
    auto&      chunkSource      = *replayDimension->mChunkSource;
    for (auto const& [pos, identity] : targetColumns) {
        if (mReusableSnapshotColumns.contains(pos) || identity.levelChunkIndex < 0) continue;
        // Applied directly, a merged seek batch left the loaded columns of a dimension it just entered empty.
        if (seekFlushedBatch) continue;
        if (!requestModeLevelChunks.contains(pos)) {
            continue;
        }

        auto covered = subChunkIndicesByColumn.find(pos);
        if (covered == subChunkIndicesByColumn.end() || covered->second.size() != subChunkCount) {
            continue;
        }
        bool const coversCompleteHeight =
            std::all_of(covered->second.begin(), covered->second.end(), [minimumSubChunk, subChunkCount](int index) {
                return index >= minimumSubChunk && static_cast<size_t>(index - minimumSubChunk) < subChunkCount;
            });
        if (!coversCompleteHeight) {
            continue;
        }

        auto chunk = chunkSource.getExistingChunk(pos);
        if (!chunk || chunk->mIsEmptyClientChunk
            || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded
            || chunk->mSubChunks->size() != subChunkCount) {
            continue;
        }
        {
            // Use the native load path until this column belongs to the current replay dimension.
            std::scoped_lock lock(mPendingLevelChunksMutex);
            auto const       retained = mRetainedReplayChunks.find(pos);
            if (retained == mRetainedReplayChunks.end() || retained->second != chunk) continue;
        }

        mDirectSnapshotColumns.emplace(pos);
        mDirectLevelChunkIndices.emplace(identity.levelChunkIndex);
    }

    std::unordered_set<ChunkPos> queuedLevelChunkPositions;
    mPendingLevelChunkIndices.clear();
    mPendingLevelChunkIndices.reserve(levelChunks.size() - mReusedSnapshotColumns);
    for (auto const& levelChunk : levelChunks) {
        if (mReusableSnapshotColumns.contains(levelChunk.pos)) continue;
        mPendingLevelChunkIndices.emplace_back(levelChunk.index);
        queuedLevelChunkPositions.emplace(levelChunk.pos);
    }

    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mCompletedLevelChunkPositions.insert(mReusableSnapshotColumns.begin(), mReusableSnapshotColumns.end());
    }

    std::stable_sort(subChunks.begin(), subChunks.end(), [](auto const& left, auto const& right) {
        return left.distanceSquared < right.distanceSquared;
    });
    mPendingSubChunkPackets.clear();
    mPendingSubChunkPackets.reserve(subChunks.size());
    for (auto& subChunk : subChunks) {
        bool const reusable =
            std::all_of(subChunk.packet.targets.begin(), subChunk.packet.targets.end(), [this](ChunkPos const& pos) {
                return mReusableSnapshotColumns.contains(pos);
            });
        if (reusable) continue;

        for (auto const& target : subChunk.packet.targets) {
            if (queuedLevelChunkPositions.contains(target)) subChunk.packet.dependencies.emplace_back(target);
            ++mRemainingSubChunkPacketsByColumn[target];
        }
        mPendingSubChunkPackets.emplace_back(std::move(subChunk.packet));
    }
    mPendingSubChunkIndices.clear();

    if (skippedBlobCachePackets != 0) {
        getLogger().warn(
            "Skipped {} replay chunk packets that depend on unavailable server blob-cache data",
            skippedBlobCachePackets
        );
    }

    mPendingSnapshotColumns = std::move(targetColumns);
    if (mApplyingChunkSnapshot || !levelChunks.empty()) {
        getLogger().debug(
            "Prepared replay {} chunk batch with {} target columns: {} reused, {} direct, {} native",
            mApplyingChunkSnapshot ? "snapshot" : "timeline",
            mPendingSnapshotColumns.size(),
            mReusedSnapshotColumns,
            mDirectSnapshotColumns.size(),
            levelChunks.size() - mReusedSnapshotColumns - mDirectSnapshotColumns.size()
        );
    }

    mPendingLevelChunkCursor    = 0;
    mPendingSubChunkCursor      = 0;
    std::string const centerKey = std::format(
        "{}|{}|{}|{}|{}|{}|{}",
        followCenter ? "follow" : "snapshot",
        followSubject,
        replayDimension->getDimensionId().mValue,
        centerX,
        centerZ,
        snapshotChunkX,
        snapshotChunkZ
    );
    if (centerKey != mChunkCenterLogKey) {
        mChunkCenterLogKey = centerKey;
        getLogger().debug(
            "replay chunk center center={} subject={} dimension={} centerChunk={},{} snapshotChunk={},{} planned={} "
            "injected={} skipped={}",
            followCenter ? "follow" : "snapshot",
            followSubject.empty() ? "-" : followSubject,
            replayDimension->getDimensionId().mValue,
            centerX,
            centerZ,
            snapshotChunkX,
            snapshotChunkZ,
            levelChunks.size(),
            mPendingLevelChunkIndices.size(),
            skippedBlobCachePackets + skippedDimensionPackets
        );
    }
    mChunkInjectionPlanPrepared = true;
    return true;
}

bool ReplaySession::tryFinishChunkInjection(std::optional<std::chrono::steady_clock::time_point> catchUpDeadline) {
    if (!mChunkInjectionPending) return true;
    if (mSnapshotGamePacketPhase == SnapshotGamePacketPhase::WaitingAfterPlayerList) {
        // A seek is waiting on these, so it drains them in one pass instead of spreading them over client ticks.
        bool const seeking = mSeekTargetTick >= 0 && !mExportSeekRequested;
        auto const deadline =
            std::chrono::steady_clock::now() + (seeking ? SeekEntityPacketBudget : SnapshotGamePacketBudget);
        if (!flushPendingSnapshotGamePackets(
                false,
                seeking ? std::numeric_limits<size_t>::max() : SNAPSHOT_GAME_PACKETS_PER_TICK,
                deadline
            )) {
            mReplayFailed = true;
            return false;
        }
        if (!mPendingSnapshotGamePackets.empty()) {
            return false;
        }
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::WaitingAfterEntities;
        return false;
    }
    if (mSnapshotGamePacketPhase == SnapshotGamePacketPhase::WaitingAfterEntities) {
        mAppliedSnapshotColumns = std::move(mPendingSnapshotColumns);
        mDirtySnapshotColumns.clear();
        mReusableSnapshotColumns.clear();
        mDirectSnapshotColumns.clear();
        mDirectLevelChunkIndices.clear();
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
        mChunkInjectionPending   = false;
        return true;
    }
    if (!mChunkInjectionPlanPrepared) {
        auto* player = mReplayPlayer;
        if (!player) {
            getLogger().error("Unable to finish replay chunk injection: replay player is unavailable");
            mReplayFailed = true;
            return false;
        }
        auto const&  rotation       = player->getRotation();
        auto const&  playerPosition = player->getPosition();
        auto const   position = mExportTimelinePhase != ReplayExportTimelinePhase::Inactive && mExportCameraViewpoint
                                  ? *mExportCameraViewpoint
                                  : ReplayCameraViewpoint{playerPosition.x, playerPosition.y, playerPosition.z};
        PlaybackView view{position.x, position.y, position.z, rotation.y, rotation.x};
        mChunkInjectionStartedAt = std::chrono::steady_clock::now();
        if (!prepareChunkInjectionPlan(view)) {
            getLogger().error("Unable to prepare the replay chunk injection plan");
            mReplayFailed = true;
            return false;
        }
        mChunkPlanPreparationMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt)
                .count();
    }

    bool const   completionProgress = mChunkCompletionObserved.exchange(false, std::memory_order_acq_rel);
    size_t const levelCursorBefore  = mPendingLevelChunkCursor;
    size_t const subCursorBefore    = mPendingSubChunkCursor;
    auto const   injectionStarted   = std::chrono::steady_clock::now();
    // A seek shares one deadline across its whole catch-up; normal playback keeps the per-tick frame budget.
    auto const perTickBudget           = mCenterChunksReady ? OuterChunkInjectionBudget : CenterChunkInjectionBudget;
    auto const deadline                = catchUpDeadline ? *catchUpDeadline : injectionStarted + perTickBudget;
    size_t     injectedSubChunkPackets = 0;
    if (!injectReadySubChunkPackets(injectedSubChunkPackets, deadline) || !injectPendingLevelChunks(deadline)) {
        return false;
    }
    // Each SubChunk pass can unblock later ones, so keep going while a pass still injects something.
    for (;;) {
        size_t const before = injectedSubChunkPackets;
        if (!injectReadySubChunkPackets(injectedSubChunkPackets, deadline)) {
            return false;
        }
        if (injectedSubChunkPackets == before) break;
    }
    updateCenterChunkReadiness();
    mChunkInjectionDurationsMs.emplace_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - injectionStarted).count()
    );

    size_t completedAfter;
    size_t inFlight;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completedAfter = mCompletedLevelChunkPositions.size();
        inFlight       = mPendingLevelChunks.size();
    }

    bool const allLevelsInjected    = mPendingLevelChunkCursor >= mPendingLevelChunkIndices.size();
    bool const allSubChunksInjected = mPendingSubChunkCursor >= mPendingSubChunkPackets.size();
    if (allLevelsInjected && inFlight == 0 && allSubChunksInjected) return finishChunkInjection();

    bool const madeProgress = completionProgress || mPendingLevelChunkCursor != levelCursorBefore
                           || mPendingSubChunkCursor != subCursorBefore;
    if (madeProgress) mChunkInjectionIdleTicks = 0;
    else ++mChunkInjectionIdleTicks;

    if (mChunkInjectionTicks == 1 || mChunkInjectionTicks % 20 == 0) {
        getLogger().debug(
            "Streaming replay chunks: LevelChunk {}/{} queued, {} completed, {} in flight; SubChunk {}/{} injected",
            mPendingLevelChunkCursor,
            mPendingLevelChunkIndices.size(),
            completedAfter,
            inFlight,
            mPendingSubChunkCursor,
            mPendingSubChunkPackets.size()
        );
        getLogger().flush();
    }
    if (mChunkInjectionIdleTicks >= CHUNK_INJECTION_STALL_TIMEOUT_TICKS) {
        getLogger().error(
            "Replay chunk streaming made no progress for {} ticks ({} LevelChunks in flight, SubChunk {}/{})",
            mChunkInjectionIdleTicks,
            inFlight,
            mPendingSubChunkCursor,
            mPendingSubChunkPackets.size()
        );
        mReplayFailed = true;
    }
    return false;
}

bool ReplaySession::injectPendingLevelChunks(std::chrono::steady_clock::time_point deadline) {
    size_t       processed = 0;
    size_t const inFlightLimit =
        mSeekTargetTick >= 0 && !mExportSeekRequested ? SEEK_LEVEL_CHUNKS_IN_FLIGHT : MAX_LEVEL_CHUNKS_IN_FLIGHT;
    while (mPendingLevelChunkCursor < mPendingLevelChunkIndices.size()) {
        if (processed != 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }

        int const  index  = mPendingLevelChunkIndices[mPendingLevelChunkCursor];
        bool const direct = mDirectLevelChunkIndices.contains(index);
        if (!direct) {
            std::scoped_lock lock(mPendingLevelChunksMutex);
            if (mPendingLevelChunks.size() >= inFlightLimit) {
                break;
            }
        }

        bool applied = false;
        if (direct) {
            applied = applyRequestModeLevelChunkDirect(mChunkPackets[static_cast<size_t>(index)]);
            if (!applied) {
                getLogger().debug("Direct replay LevelChunk update unavailable; using native loading");
                mDirectLevelChunkIndices.clear();
                mDirectSnapshotColumns.clear();
                continue;
            }
        } else {
            applied = injectChunkPacket(mChunkPackets[static_cast<size_t>(index)], MinecraftPacketIds::FullChunkData);
        }
        if (!applied) {
            getLogger().error(
                "Unable to {} replay LevelChunk packet {} of {}",
                direct ? "apply directly" : "inject",
                mPendingLevelChunkCursor,
                mPendingLevelChunkIndices.size()
            );
            mReplayFailed = true;
            return false;
        }
        ++mPendingLevelChunkCursor;
        ++processed;
    }
    return true;
}

bool ReplaySession::injectReadySubChunkPackets(
    size_t&                               injectedPackets,
    std::chrono::steady_clock::time_point deadline
) {
    // Injection completes columns synchronously, so re-read rather than snapshotting the set once up front.
    auto dependenciesMet = [this](std::vector<ChunkPos> const& dependencies) {
        if (dependencies.empty()) return true;
        std::scoped_lock lock(mPendingLevelChunksMutex);
        return std::all_of(dependencies.begin(), dependencies.end(), [this](ChunkPos const& pos) {
            return mCompletedLevelChunkPositions.contains(pos);
        });
    };

    for (auto& pending : mPendingSubChunkPackets) {
        if (pending.injected) continue;
        if (std::chrono::steady_clock::now() >= deadline) break;
        if (!dependenciesMet(pending.dependencies)) {
            continue;
        }
        bool const direct = std::all_of(pending.targets.begin(), pending.targets.end(), [this](ChunkPos const& pos) {
            return mDirectSnapshotColumns.contains(pos) || mReusableSnapshotColumns.contains(pos);
        });
        std::string_view payload = pending.payload.empty()
                                     ? std::string_view{mChunkPackets[static_cast<size_t>(pending.index)]}
                                     : std::string_view{pending.payload};
        bool             applied =
            direct ? applySubChunkDirect(payload) : injectChunkPacket(payload, MinecraftPacketIds::SubChunkPacket);
        if (direct && !applied) {
            getLogger().debug("Direct replay SubChunk update unavailable; using native loading");
            for (auto const& target : pending.targets) mDirectSnapshotColumns.erase(target);
            applied = injectChunkPacket(payload, MinecraftPacketIds::SubChunkPacket);
        }
        if (!applied) {
            getLogger()
                .error("Unable to {} replay SubChunk packet {}", direct ? "apply directly" : "inject", pending.index);
            mReplayFailed = true;
            return false;
        }

        pending.injected = true;
        ++mPendingSubChunkCursor;
        ++injectedPackets;
        for (auto const& target : pending.targets) {
            auto remaining = mRemainingSubChunkPacketsByColumn.find(target);
            if (remaining != mRemainingSubChunkPacketsByColumn.end() && remaining->second != 0) {
                --remaining->second;
            }
        }
    }
    return true;
}

void ReplaySession::updateCenterChunkReadiness() {
    if (mCenterChunksReady || mCenterChunkPositions.empty()) return;

    std::unordered_set<ChunkPos> completed;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completed = mCompletedLevelChunkPositions;
    }
    for (auto const& pos : mCenterChunkPositions) {
        if (!completed.contains(pos)) return;
        auto remaining = mRemainingSubChunkPacketsByColumn.find(pos);
        if (remaining != mRemainingSubChunkPacketsByColumn.end() && remaining->second != 0) return;
    }

    mCenterChunksReady = true;
    auto const elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt);
    size_t const queuedCenterColumns = static_cast<size_t>(
        std::count_if(mCenterChunkPositions.begin(), mCenterChunkPositions.end(), [this](ChunkPos const& pos) {
            return !mReusableSnapshotColumns.contains(pos);
        })
    );
    size_t const queuedOuterColumns = mPendingLevelChunkIndices.size() - queuedCenterColumns;
    getLogger().debug(
        "Replay center ready with {} columns in {:.3f} ms after {} ticks; streaming {} outer columns",
        mCenterChunkPositions.size(),
        elapsed.count(),
        mChunkInjectionTicks,
        queuedOuterColumns
    );
}

bool ReplaySession::finishChunkInjection() {
    // Native request-mode loading can reuse a column without replacing its biomes.
    // Apply only the last packet per column, after all native work has finished.
    for (int index : mPendingLevelChunkIndices) {
        if (mDirectLevelChunkIndices.contains(index)) continue;
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
        if (!packet) {
            mReplayFailed = true;
            return false;
        }
        ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(index)], false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) {
            mReplayFailed = true;
            return false;
        }
        auto const& levelChunk = static_cast<LevelChunkPacket const&>(*packet);
        if (levelChunk.mClientRequestSubChunkLimit->has_value() && !applyRequestModeLevelChunkData(levelChunk)) {
            auto const& pos = *levelChunk.mPos;
            getLogger().error("Unable to restore replay chunk metadata at ({}, {})", pos.x, pos.z);
            mReplayFailed = true;
            return false;
        }
    }
    updateCenterChunkReadiness();
    bool const applyingSnapshot = mApplyingChunkSnapshot;

    size_t completedLevelChunks;
    size_t retainedReplayChunks;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completedLevelChunks = mCompletedLevelChunkPositions.size();
        retainedReplayChunks = mRetainedReplayChunks.size();
    }
    auto const elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt);
    double injectionP95Ms = 0.0;
    double injectionMaxMs = 0.0;
    if (!mChunkInjectionDurationsMs.empty()) {
        auto sortedDurations = mChunkInjectionDurationsMs;
        std::sort(sortedDurations.begin(), sortedDurations.end());
        size_t const p95Index = (sortedDurations.size() * 95 + 99) / 100 - 1;
        injectionP95Ms        = sortedDurations[p95Index];
        injectionMaxMs        = sortedDurations.back();
    }

    if (applyingSnapshot) {
        mSnapshotChunks        = std::move(mApplyingSnapshotChunks);
        mApplyingChunkSnapshot = false;
    }

    if (applyingSnapshot
        && !flushPendingSnapshotGamePackets(
            true,
            std::numeric_limits<size_t>::max(),
            std::chrono::steady_clock::time_point::max()
        )) {
        mReplayFailed = true;
        return false;
    }
    if (applyingSnapshot && !applyPendingSnapshotLocalPlayer()) {
        mReplayFailed = true;
        return false;
    }

    if (applyingSnapshot) {
        getLogger().debug(
            "Applied replay snapshot in {:.3f} ms after {} ticks with {} reused columns, {} direct and {} native "
            "LevelChunks ({} completed), and {} direct SubChunk packets ({} entries) plus {} native packets ({} "
            "entries); plan {:.3f} ms, injection tick p95 {:.3f} ms, max {:.3f} ms",
            elapsed.count(),
            mChunkInjectionTicks,
            mReusedSnapshotColumns,
            mDirectLevelChunks,
            mInjectedLevelChunks,
            completedLevelChunks,
            mDirectSubChunkPackets,
            mDirectSubChunkEntries,
            mInjectedSubChunkPackets,
            mInjectedSubChunkEntries,
            mChunkPlanPreparationMs,
            injectionP95Ms,
            injectionMaxMs
        );
    } else if (mInjectedLevelChunks != 0 || mDirectLevelChunks != 0) {
        getLogger().debug(
            "Applied replay timeline chunks in {:.3f} ms after {} ticks with {} LevelChunks and {} SubChunk "
            "packets ({} entries); {} replay columns retained",
            elapsed.count(),
            mChunkInjectionTicks,
            mInjectedLevelChunks,
            mInjectedSubChunkPackets,
            mInjectedSubChunkEntries,
            retainedReplayChunks
        );
    } else {
        getLogger().debug(
            "Applied replay timeline SubChunks in {:.3f} ms after {} ticks with {} packets ({} entries)",
            elapsed.count(),
            mChunkInjectionTicks,
            mInjectedSubChunkPackets,
            mInjectedSubChunkEntries
        );
    }

    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mSeekFlushedBatch = false;
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
    }
    mPendingLevelChunkCursor    = 0;
    mPendingSubChunkCursor      = 0;
    mChunkInjectionTicks        = 0;
    mChunkInjectionIdleTicks    = 0;
    mChunkInjectionPlanPrepared = false;
    mSnapshotGamePacketPhase =
        applyingSnapshot ? SnapshotGamePacketPhase::WaitingAfterPlayerList : SnapshotGamePacketPhase::StreamingChunks;
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;

    if (!applyingSnapshot) {
        // A seek re-sends the same columns repeatedly, so record what landed; dirty marks must survive for replay.
        for (auto& [pos, identity] : mPendingSnapshotColumns) {
            if (identity.levelChunkIndex < 0) mDirtySnapshotColumns.emplace(pos);
            else mAppliedSnapshotColumns.insert_or_assign(pos, std::move(identity));
        }
        mPendingSnapshotColumns.clear();
        mReusableSnapshotColumns.clear();
        mDirectSnapshotColumns.clear();
        mDirectLevelChunkIndices.clear();
        mInjectedLevelChunks     = 0;
        mInjectedSubChunkPackets = 0;
        mInjectedSubChunkEntries = 0;
        mReusedSnapshotColumns   = 0;
        mDirectLevelChunks       = 0;
        mDirectSubChunkPackets   = 0;
        mDirectSubChunkEntries   = 0;
        mChunkInjectionPending   = false;
        // Block updates held behind a merged seek batch go on top of the data that just landed.
        if (!mSeekReleasedPackets.empty() && !releaseSeekPackets()) {
            mReplayFailed = true;
            return false;
        }
        return true;
    }

    return false;
}

void ReplaySession::handleNextTick() {
    if (mIsProcessingSnapshot) {
        throw std::runtime_error("Can't go to next tick while processing snapshot");
    }
    // A tick a player did not move still has to settle that player's map bob; the last step would otherwise stick.
    if (mReplayPlayer) {
        auto& level = mReplayPlayer->getLevel();
        for (auto& [rawId, bobTick] : gReplayHandBobTick) {
            if (bobTick == mCurrentTick) continue;
            ActorUniqueID const id{rawId};
            if (!mRecordedEntityIds.contains(id)) continue;
            auto* actor = level.fetchEntity(id, false);
            if (!actor || actor == mReplayPlayer || !actor->isPlayer()) continue;
            updateReplayHandBob(*actor, 0.0f);
            bobTick = mCurrentTick;
        }
    }
    visuals::commitReplayEntityPoses(static_cast<int64_t>(mCurrentTick) + 1);
    mCurrentTick += 1;
    integration::pushReplayClock(mCurrentTick, false);
    if (mReplayTime) {
        ++*mReplayTime;
        if (mReplayPlayer) mReplayPlayer->getLevel().setTime(*mReplayTime);
    }
    updateReplaySubjectPosture(false);
}

void ReplaySession::handleSnapshotContext(PlaybackSnapshotContext const& context) {
    if (!mIsProcessingSnapshot) {
        throw std::runtime_error("Snapshot context appeared outside a replay snapshot");
    }
    if (mReaderIndex >= mSnapshotContexts.size() || context != mSnapshotContexts[mReaderIndex]) {
        throw std::runtime_error("Replay snapshot context changed after preflight");
    }
}

void ReplaySession::handleCreateLocalPlayer(PlaybackBuffer& data) {
    auto const  remaining = data.getWritePointer() - data.mReadPointer;
    std::string payload(data.mView.data() + data.mReadPointer, remaining);
    data.mReadPointer += remaining;

    if (mIsProcessingSnapshot) {
        if (mPendingSnapshotLocalPlayer) {
            getLogger().error("Replay snapshot {} contains more than one CreateLocalPlayer action", mReaderIndex);
            mReplayFailed = true;
            return;
        }
        mPendingSnapshotLocalPlayer = std::move(payload);
        return;
    }

    if (!applyGamePacket(MinecraftPacketIds::AddPlayer, payload, true)) mReplayFailed = true;
}

bool ReplaySession::sendRecordedTickPacket() {
    if (!mActive || !mWorldReady || mIsPaused) return false;

    return advanceReplayTick(true);
}

bool ReplaySession::hasPendingReplayReaderBoundary() const {
    return mReaderIndex < mReaders.size() && !mReaders[mReaderIndex]->hasRemainingActions();
}

bool ReplaySession::advanceReplayReader(bool stopAtEnd) {
    ++mReaderIndex;
    if (mReaderIndex >= mReaders.size()) {
        if (stopAtEnd) {
            mIsPaused                = true;
            mPlaybackTickAccumulator = 0.0f;
            getLogger().debug("Replay finished and paused at tick {}", mCurrentTick);
        }
        return false;
    }

    auto chunkMeta = mMeta.chunks.begin();
    std::advance(chunkMeta, static_cast<std::ptrdiff_t>(mReaderIndex));
    if (chunkMeta->second.forcePlaySnapshot) {
        applySnapshot(*mReaders[mReaderIndex], false);
    } else {
        mReaders[mReaderIndex]->resetToStart();
    }
    if (mReplayFailed) throw std::runtime_error("Unable to apply a replay action");
    return true;
}

bool ReplaySession::advanceReplayTick(bool stopAtEnd) {
    if (!mActive || !mWorldReady) return false;

    int const startingTick = mCurrentTick;
    while (mActive && mCurrentTick == startingTick) {
        if (mReaderIndex >= mReaders.size()) {
            if (stopAtEnd) {
                mIsPaused                = true;
                mPlaybackTickAccumulator = 0.0f;
                getLogger().debug("Replay finished and paused at tick {}", mCurrentTick);
            }
            return false;
        }

        auto& reader = mReaders[mReaderIndex];
        if (!reader->handleNextAction(*this)) {
            return advanceReplayReader(stopAtEnd);
        }

        if (mPendingReplayDimension) return true;
        // The rest of the tick resumes from the same action once the batch an entity needs has landed.
        if (mSeekTickInterrupted) return true;

        if (mReplayFailed) throw std::runtime_error("Unable to apply a replay action");
    }
    if (mActive && mReplayPlayer) {
        auto& level = mReplayPlayer->getLevel();
        mHeldItemUse.clear();
        for (auto const& id : mRecordedEntityIds) {
            auto* actor = level.fetchEntity(id, false);
            if (!actor || !actor->isPlayer()) continue;
            tickReplayItemUse(*actor);
            if (auto inUse = actor->getEntityContext().tryGetComponent<ItemInUseComponent>()) {
                int const duration = inUse->mDuration;
                mHeldItemUse[id]   = {static_cast<void const*>(inUse->mItem.get().get()), duration};
            }
        }
    }
    return true;
}

// The native pause flag stops animation and particle updates; the replay drives entity state itself.
void ReplaySession::syncLevelSimPause(bool freeze) {
    if (mLevelSimFrozen == freeze || !mReplayPlayer) return;
    mReplayPlayer->getLevel().setSimPaused(freeze);
    mLevelSimFrozen = freeze;
}

// The native pause flag leaves item-use ticking alone, which runs the use animation on between replay ticks.
void ReplaySession::holdItemUseBetweenTicks() {
    if (!mReplayPlayer) return;
    auto& level = mReplayPlayer->getLevel();
    for (auto const& id : mRecordedEntityIds) {
        auto* actor = level.fetchEntity(id, false);
        if (!actor || !actor->isPlayer()) continue;
        auto inUse = actor->getEntityContext().tryGetComponent<ItemInUseComponent>();
        if (!inUse) {
            mHeldItemUse.erase(id);
            continue;
        }
        int&        duration = inUse->mDuration;
        void const* item     = static_cast<void const*>(inUse->mItem.get().get());
        auto const  held     = mHeldItemUse.find(id);
        // A new item or a snapshot-reloaded use has no tick to hold yet; take its current state as the baseline.
        if (held == mHeldItemUse.end() || held->second.item != item || duration > held->second.duration) {
            mHeldItemUse[id] = {item, duration};
            continue;
        }
        if (duration < held->second.duration) duration = held->second.duration;
    }
}

void ReplaySession::handleLevelChunkCached(int index) {
    if (index < 0 || static_cast<size_t>(index) >= mChunkPackets.size()) {
        mReplayFailed = true;
        return;
    }
    if (deferSeekChunk(index, true)) return;
    if (!mChunkInjectionPending) {
        mChunkInjectionTicks     = 0;
        mChunkInjectionIdleTicks = 0;
        mCenterChunksReady       = false;
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    }
    mPendingLevelChunkIndices.emplace_back(index);
    mChunkInjectionPending      = true;
    mChunkInjectionPlanPrepared = false;
}

void ReplaySession::handleSubChunkCached(int index) {
    if (index < 0 || static_cast<size_t>(index) >= mChunkPackets.size()) {
        mReplayFailed = true;
        return;
    }
    if (deferSeekChunk(index, false)) return;
    if (!mChunkInjectionPending) {
        mChunkInjectionTicks     = 0;
        mChunkInjectionIdleTicks = 0;
        mCenterChunksReady       = false;
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    }
    mPendingSubChunkIndices.emplace_back(index);
    mChunkInjectionPending      = true;
    mChunkInjectionPlanPrepared = false;
}

// Each timeline batch stops catch-up until the client finishes loading it, and a seek crosses many batches that
// re-send the same columns. Held back, they load as one batch with only the latest data per column.
bool ReplaySession::seekChunkDeferralOpen() const {
    if (mSeekTargetTick < 0 || mExportSeekRequested || mIsProcessingSnapshot || mApplyingChunkSnapshot
        || mPendingReplayDimension) {
        return false;
    }
    // A batch already loading is left to finish; holding starts again once it lands.
    return mSeekChunksDeferred || !mChunkInjectionPending;
}

bool ReplaySession::deferSeekChunk(int index, bool levelChunk) {
    // A deferral cannot outlive the conditions that close it, or the held data would land over newer packets.
    if (mSeekChunksDeferred
        && (mExportSeekRequested || mIsProcessingSnapshot || mApplyingChunkSnapshot || mPendingReplayDimension)) {
        flushSeekChunks();
        if (mReplayFailed) return false;
    }
    if (!seekChunkDeferralOpen()) return false;
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    auto packet = MinecraftPackets::createPacket(
        levelChunk ? MinecraftPacketIds::FullChunkData : MinecraftPacketIds::SubChunkPacket
    );
    if (!packet) return false;
    ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(index)], false);
    // The normal path reports packets that do not decode.
    if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) return false;

    auto const sequence = ++mSeekDeferredSequence;
    if (levelChunk) {
        auto const& chunk   = static_cast<LevelChunkPacket const&>(*packet);
        bool const  skipped = static_cast<bool>(chunk.mCacheEnabled)
                          || static_cast<DimensionType const&>(chunk.mDimensionId) != replayDimension->getDimensionId();
        ChunkPos const pos = *chunk.mPos;
        mSeekDeferredLevelChunkPackets.push_back(SeekDeferredLevelChunk{index, sequence, pos, skipped});
        if (!skipped) {
            mSeekLatestLevelChunks.insert_or_assign(pos, sequence);
            // 26.40 folded the request-mode flag into the limit: a value means the client must request subchunks.
            if (!chunk.mClientRequestSubChunkLimit->has_value()) {
                mSeekLatestBlockLevelChunks.insert_or_assign(pos, sequence);
            }
        }
    } else {
        auto const& subChunk = static_cast<SubChunkPacket const&>(*packet);
        bool const  skipped =
            static_cast<bool>(subChunk.mCacheEnabled)
            || static_cast<DimensionType const&>(subChunk.mDimensionType) != replayDimension->getDimensionId();
        SeekDeferredSubChunk deferred{index, sequence, skipped, {}};
        if (!skipped) {
            auto const& center = *subChunk.mCenterPos;
            for (auto const& entry : *subChunk.mSubChunkData) {
                auto const&    offset = *entry.mSubChunkPosOffset;
                ChunkPos const column{center.x + static_cast<int>(offset.mX), center.z + static_cast<int>(offset.mZ)};
                int const      y          = center.y + static_cast<int>(offset.mY);
                auto const     result     = static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult);
                bool const     successful = isSuccessfulSubChunkResult(result);
                deferred.entries.push_back(SeekDeferredSubChunkEntry{column, y, successful});
                if (successful) mSeekLatestSubChunks[column].insert_or_assign(y, sequence);
            }
        }
        mSeekDeferredSubChunkPackets.push_back(std::move(deferred));
    }
    mSeekChunksDeferred = true;
    return true;
}

// A block update for a held column would land under the older chunk data, so it waits for that data.
bool ReplaySession::holdSeekBlockPacket(Packet const& packet, std::string_view payload) {
    std::optional<BlockPos> target;
    switch (packet.getId()) {
    case MinecraftPacketIds::UpdateBlock:
        target = *static_cast<UpdateBlockPacket const&>(packet).mPos;
        break;
    case MinecraftPacketIds::UpdateBlockSynced:
        target = *static_cast<UpdateBlockSyncedPacket const&>(packet).mPos;
        break;
    case MinecraftPacketIds::UpdateSubChunkBlocks: {
        // The entries carry block positions of one sub chunk; the header position is not guaranteed to be one.
        auto const& subChunk = static_cast<UpdateSubChunkBlocksPacket const&>(packet);
        auto const& changed  = *subChunk.mBlocksChanged;
        if (!changed.mStandards->empty()) target = *changed.mStandards->front().mPos;
        else if (!changed.mExtras->empty()) target = *changed.mExtras->front().mPos;
        else target = *subChunk.mSubChunkBlockPosition;
        break;
    }
    case MinecraftPacketIds::BlockActorData:
        target = *static_cast<BlockActorDataPacket const&>(packet).mPos;
        break;
    case MinecraftPacketIds::TileEvent:
        target = *static_cast<BlockEventPacket const&>(packet).mPos;
        break;
    default:
        break;
    }
    if (!target) return false;

    ChunkPos const column{target->x >> 4, target->z >> 4};
    int const      y    = target->y >> 4;
    bool           held = mSeekLatestLevelChunks.contains(column);
    if (!held) {
        auto const subChunks = mSeekLatestSubChunks.find(column);
        held                 = subChunks != mSeekLatestSubChunks.end() && subChunks->second.contains(y);
    }
    if (!held) return false;

    // A dropped update still leaves the column changed since the snapshot that a later seek may reuse.
    invalidateSnapshotColumns(packet);
    mSeekHeldBlockPackets.push_back(
        SeekHeldBlockPacket{packet.getId(), std::string(payload), column, y, ++mSeekDeferredSequence}
    );
    return true;
}

void ReplaySession::flushSeekChunks() {
    if (!mSeekChunksDeferred) return;

    // Data older than a later packet for the same place is what the client would have overwritten anyway.
    auto const levelChunkAfter = [this](ChunkPos const& column, uint64_t sequence) {
        auto const latest = mSeekLatestLevelChunks.find(column);
        return latest != mSeekLatestLevelChunks.end() && latest->second > sequence;
    };
    auto const blockLevelChunkAfter = [this](ChunkPos const& column, uint64_t sequence) {
        auto const latest = mSeekLatestBlockLevelChunks.find(column);
        return latest != mSeekLatestBlockLevelChunks.end() && latest->second > sequence;
    };
    auto const subChunkAfter = [this](ChunkPos const& column, int y, uint64_t sequence) {
        auto const subChunks = mSeekLatestSubChunks.find(column);
        if (subChunks == mSeekLatestSubChunks.end()) return false;
        auto const latest = subChunks->second.find(y);
        return latest != subChunks->second.end() && latest->second > sequence;
    };

    std::vector<int> levelIndices;
    levelIndices.reserve(mSeekDeferredLevelChunkPackets.size());
    for (auto const& deferred : mSeekDeferredLevelChunkPackets) {
        if (!deferred.passthrough && levelChunkAfter(deferred.pos, deferred.sequence)) continue;
        levelIndices.push_back(deferred.index);
    }

    std::vector<int> subIndices;
    subIndices.reserve(mSeekDeferredSubChunkPackets.size());
    for (auto const& deferred : mSeekDeferredSubChunkPackets) {
        if (deferred.passthrough) {
            subIndices.push_back(deferred.index);
            continue;
        }
        // Most packets are either fully current or fully replaced, so only a partial overlap is decoded again.
        size_t successful = 0;
        size_t superseded = 0;
        for (auto const& place : deferred.entries) {
            if (!place.successful) continue;
            ++successful;
            if (blockLevelChunkAfter(place.column, deferred.sequence)
                || subChunkAfter(place.column, place.subChunk, deferred.sequence)) {
                ++superseded;
            }
        }
        if (superseded == 0 && successful > 0) {
            subIndices.push_back(deferred.index);
            continue;
        }
        if (superseded == successful) continue;
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
        if (!packet) {
            mReplayFailed = true;
            return;
        }
        ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(deferred.index)], false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) {
            mReplayFailed = true;
            return;
        }
        auto& subChunk = static_cast<SubChunkPacket&>(*packet);
        auto& entries  = *subChunk.mSubChunkData;
        if (entries.size() != deferred.entries.size()) {
            mReplayFailed = true;
            return;
        }
        std::vector<SubChunkPacket::SubChunkPacketData> kept;
        kept.reserve(entries.size() - superseded);
        for (size_t i = 0; i < entries.size(); ++i) {
            auto const& place = deferred.entries[i];
            if (place.successful
                && (blockLevelChunkAfter(place.column, deferred.sequence)
                    || subChunkAfter(place.column, place.subChunk, deferred.sequence))) {
                continue;
            }
            kept.push_back(entries[i]);
        }
        entries = std::move(kept);
        PlaybackBuffer filtered;
        subChunk.write(filtered);
        subIndices.push_back(cacheInlineChunkPacket(MinecraftPacketIds::SubChunkPacket, std::move(filtered.mBuffer)));
    }

    for (auto& held : mSeekHeldBlockPackets) {
        if (blockLevelChunkAfter(held.column, held.sequence)
            || subChunkAfter(held.column, held.subChunk, held.sequence)) {
            continue;
        }
        mSeekReleasedPackets.push_back(SeekReleasedPacket{held.id, std::move(held.payload), false});
    }

    mSeekChunksDeferred = false;
    mSeekDeferredLevelChunkPackets.clear();
    mSeekDeferredSubChunkPackets.clear();
    mSeekLatestLevelChunks.clear();
    mSeekLatestBlockLevelChunks.clear();
    mSeekLatestSubChunks.clear();
    mSeekHeldBlockPackets.clear();

    if (levelIndices.empty() && subIndices.empty()) {
        if (!releaseSeekPackets()) mReplayFailed = true;
        return;
    }
    if (!mChunkInjectionPending) {
        mChunkInjectionTicks     = 0;
        mChunkInjectionIdleTicks = 0;
        mCenterChunksReady       = false;
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    }
    // The plan lets a later index win per column, and anything already queued arrived after these were held.
    mPendingLevelChunkIndices.insert(mPendingLevelChunkIndices.begin(), levelIndices.begin(), levelIndices.end());
    mPendingSubChunkIndices.insert(mPendingSubChunkIndices.begin(), subIndices.begin(), subIndices.end());
    mSeekFlushedBatch           = true;
    mChunkInjectionPending      = true;
    mChunkInjectionPlanPrepared = false;
}

bool ReplaySession::releaseSeekPackets() {
    mSeekTickResuming    = mSeekTickInterrupted;
    mSeekTickInterrupted = false;
    auto packets         = std::move(mSeekReleasedPackets);
    mSeekReleasedPackets.clear();
    for (auto const& packet : packets) {
        if (!applyGamePacket(packet.id, packet.payload, packet.recordedLocalPlayer)) return false;
    }
    return true;
}

void ReplaySession::clearSeekChunkDeferral() {
    mSeekChunksDeferred   = false;
    mSeekDeferredSequence = 0;
    mSeekDeferredLevelChunkPackets.clear();
    mSeekDeferredSubChunkPackets.clear();
    mSeekLatestLevelChunks.clear();
    mSeekLatestBlockLevelChunks.clear();
    mSeekLatestSubChunks.clear();
    mSeekHeldBlockPackets.clear();
    mSeekReleasedPackets.clear();
    mSeekTickInterrupted = false;
    mSeekTickResuming    = false;
    mSeekWindowEntered   = false;
    mSeekFlushedBatch    = false;
}

int ReplaySession::cacheInlineChunkPacket(MinecraftPacketIds packetId, std::string payload) {
    auto& packetIndices =
        packetId == MinecraftPacketIds::FullChunkData ? mInlineLevelChunkPacketIndices : mInlineSubChunkPacketIndices;
    auto const hash    = std::hash<std::string_view>{}(payload);
    auto&      indices = packetIndices[hash];
    for (int index : indices) {
        if (index >= 0 && static_cast<size_t>(index) < mChunkPackets.size()
            && mChunkPackets[static_cast<size_t>(index)] == payload) {
            return index;
        }
    }

    auto const index = static_cast<int>(mChunkPackets.size());
    mChunkPackets.emplace_back(std::move(payload));
    indices.emplace_back(index);
    return index;
}

void ReplaySession::handleConfigurationPacket(PlaybackBuffer& data) {
    if (!mIsProcessingSnapshot) {
        throw std::runtime_error("Configuration packet appeared outside a replay snapshot");
    }

    auto const  packetIdValue = data.getVarInt().value();
    auto const  packetId      = static_cast<MinecraftPacketIds>(packetIdValue);
    auto const  remaining     = data.getWritePointer() - data.mReadPointer;
    std::string payload(data.mView.data() + data.mReadPointer, remaining);
    data.mReadPointer += remaining;

    // A newer recorder may classify as configuration a packet this build still treats as timeline; skip it.
    auto const semantics = describePacketLifecycle(packetId);
    if (!semantics.isConfiguration()) {
        if (mUnknownConfigurationPackets.insert(packetIdValue).second) {
            getLogger().warn(
                "Skipping configuration packet {} recorded with an unknown lifecycle ({} in this build)",
                packetIdValue,
                packetLifecycleName(semantics.lifecycle)
            );
        }
        return;
    }

    auto const applied = mAppliedConfigurationPackets.find(packetIdValue);
    if (!semantics.shouldReplayEverySnapshot() && applied != mAppliedConfigurationPackets.end()
        && applied->second == payload) {
        return;
    }

    if (semantics.lifecycle == PacketLifecycle::PreWorldHandshake) {
        mAppliedConfigurationPackets.insert_or_assign(packetIdValue, std::move(payload));
        return;
    }

    if (!applyGamePacket(packetId, payload)) {
        getLogger().error("Unable to apply replay configuration packet {}", packetIdValue);
        mReplayFailed = true;
        return;
    }
    mAppliedConfigurationPackets.insert_or_assign(packetIdValue, std::move(payload));
}

void ReplaySession::handleGamePacket(PlaybackBuffer& data) {

    auto        packetId  = static_cast<MinecraftPacketIds>(data.getVarInt().value());
    auto const  remaining = data.getWritePointer() - data.mReadPointer;
    std::string payload(data.mView.data() + data.mReadPointer, remaining);
    data.mReadPointer += remaining;

    if (packetId == MinecraftPacketIds::FullChunkData || packetId == MinecraftPacketIds::SubChunkPacket) {
        int const index = cacheInlineChunkPacket(packetId, std::move(payload));
        if (packetId == MinecraftPacketIds::FullChunkData) handleLevelChunkCached(index);
        else handleSubChunkCached(index);
        return;
    }

    if (mIsProcessingSnapshot) {
        if (packetId == MinecraftPacketIds::DimensionDataPacket || packetId == MinecraftPacketIds::SetTime) {
            if (!applyGamePacket(packetId, payload)) mReplayFailed = true;
            return;
        }
        mPendingSnapshotGamePackets.emplace_back(packetId, std::move(payload));
        return;
    }
    if (!applyGamePacket(packetId, payload)) mReplayFailed = true;
}

void ReplaySession::handleLocalCameraOffset(PlaybackBuffer& data) {
    float const                              x         = data.getFloat().value();
    float const                              y         = data.getFloat().value();
    float const                              z         = data.getFloat().value();
    float const                              fov       = data.getFloat().value();
    bool const                               hasView   = data.getBool().value();
    float const                              viewX     = data.getFloat().value();
    float const                              viewY     = data.getFloat().value();
    float const                              viewZ     = data.getFloat().value();
    float const                              viewYaw   = data.getFloat().value();
    float const                              viewPitch = data.getFloat().value();
    std::optional<visuals::EntityRenderPose> view;
    if (hasView)
        view = visuals::EntityRenderPose{
            {viewX, viewY, viewZ},
            viewPitch,
            viewYaw,
            viewYaw,
            viewYaw
        };
    visuals::queueReplaySubjectCameraOffset({x, y, z}, fov, view);
}
void ReplaySession::handleLocalViewSamples(PlaybackBuffer& data) {
    // The recorder writes at most 32; anything far larger is a corrupt stream.
    constexpr int MaxSamples = 1024;
    auto const    count      = data.getVarInt().value();
    if (count < 0 || count > MaxSamples) throw std::runtime_error("Local view sample count is out of range");
    std::vector<visuals::LocalViewSample> samples;
    samples.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        float const fraction = data.getFloat().value();
        float const pitch    = data.getFloat().value();
        float const yaw      = data.getFloat().value();
        samples.push_back({fraction, pitch, yaw});
    }

    if (!mRecordedLocalPlayerId || !mReplayPlayer) return;
    auto* actor = mReplayPlayer->getLevel().fetchEntity(*mRecordedLocalPlayerId, false);
    // The observer is the free camera; its view is not the recording's.
    if (!actor || (!mIsPaused && actor == mReplayPlayer)) return;
    auto const renderKey = replayEntityRenderKey(actor->getLevel(), *mRecordedLocalPlayerId);
    if (!renderKey) return;
    visuals::queueReplayLocalViewSamples(*renderKey, std::move(samples));
}


void ReplaySession::handleMoveEntities(PlaybackBuffer& data) {

    auto dispatchMovementPacket = [this](std::shared_ptr<Packet>& packet) {
        if (!packet || !mNetworkHandler || !packet->mHandler) {
            mReplayFailed = true;
            return false;
        }
        mInjectingPacket.store(packet.get(), std::memory_order_release);
        InjectionReset reset{mInjectingPacket};
        packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
        return true;
    };

    bool const observerIsFree = !mIsPaused;

    auto const markerOrCount = data.getVarInt().value();
    if (markerOrCount == 0) {
        auto const count = data.getVarInt().value();
        if (count < 0) throw std::runtime_error("Entity movement count cannot be negative");

        bool const snapMovement = mSeekTargetTick >= 0 && mSnapMovementDuringSeek;
        for (int index = 0; index < count; ++index) {
            ActorUniqueID const id{data.getVarInt64().value()};
            Vec3 const          position{data.getFloat().value(), data.getFloat().value(), data.getFloat().value()};
            Vec2 const          rotation{data.getFloat().value(), data.getFloat().value()};
            float const         headYaw  = data.getFloat().value();
            float const         bodyYaw  = data.getFloat().value();
            bool const          onGround = data.getBool().value();

            if (!mRecordedEntityIds.contains(id) || !mReplayPlayer) continue;
            auto* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
            if (!actor) continue;
            // The observer is the free camera; skip its recorded movement.
            if (observerIsFree && actor == mReplayPlayer) {
                continue;
            }

            auto const previousPose  = replayRenderPose(*actor);
            auto const renderKey     = replayEntityRenderKey(actor->getLevel(), id);
            auto&      entityContext = actor->getEntityContext();
            auto const currentPose   = visuals::EntityRenderPose{
                replayRenderPosition(position),
                rotation.x,
                rotation.y,
                headYaw,
                bodyYaw,
            };

            if (renderKey) {
                mEntityRenderKeys.insert_or_assign(id, *renderKey);
                visuals::queueReplayEntityPose(*renderKey, previousPose, currentPose);
            }

            if (actor->isRiding(nullptr)) {
                actor->mBuiltInComponents->mActorRotationComponent->mRot     = rotation;
                actor->mBuiltInComponents->mActorRotationComponent->mRotPrev = rotation;
                pinReplayEntityHeadRotation(*actor, headYaw, bodyYaw);

                if (onGround) {
                    if (!entityContext.hasComponent<OnGroundFlagComponent>()) {
                        entityContext.getRegistry().emplace<OnGroundFlagComponent>(entityContext.mEntity);
                    }
                } else {
                    entityContext.removeComponent<OnGroundFlagComponent>();
                }
                if (snapMovement) cancelNativeMovementInterpolation(*actor, position, rotation, headYaw);
                else applyReplayEntityMovement(*actor, position, previousPose, rotation, headYaw, mCurrentTick);
                continue;
            }

            std::shared_ptr<Packet> packet;
            if (actor->isPlayer()) {
                packet = MinecraftPackets::createPacket(MinecraftPacketIds::MovePlayer);
                if (packet) {
                    auto& move          = static_cast<MovePlayerPacket&>(*packet);
                    move.mPlayerID      = actor->getRuntimeID();
                    move.mPos           = position;
                    move.mRot           = rotation;
                    move.mYHeadRot      = headYaw;
                    move.mResetPosition = PlayerPositionModeComponent::PositionMode::Teleport;
                    move.mTeleportData  = MovePlayerTeleportData{0, -1};
                    move.mOnGround      = onGround;
                }
            } else {
                auto quantizeRotation = [](float degrees) {
                    return static_cast<schar>(std::floor(degrees * 256.0f / 360.0f));
                };

                packet = MinecraftPackets::createPacket(MinecraftPacketIds::MoveAbsoluteActor);
                if (packet) {
                    auto& move         = *static_cast<MoveActorAbsolutePacket&>(*packet).mMoveData;
                    move.mRuntimeId    = actor->getRuntimeID();
                    move.mHeader->mRaw = static_cast<uchar>((onGround ? 1 : 0) | 2);
                    move.mPos          = position;
                    move.mRotX         = quantizeRotation(rotation.x);
                    move.mRotY         = quantizeRotation(rotation.y);
                    move.mRotYHead     = quantizeRotation(headYaw);
                    move.mRotYBody     = quantizeRotation(bodyYaw);
                }
            }

            if (!dispatchMovementPacket(packet)) return;

            pinReplayEntityHeadRotation(*actor, headYaw, bodyYaw);
            if (snapMovement) cancelNativeMovementInterpolation(*actor, position, rotation, headYaw);
            else applyReplayEntityMovement(*actor, position, previousPose, rotation, headYaw, mCurrentTick);
        }
        return;
    }

    auto const count = markerOrCount;
    if (count < 0) throw std::runtime_error("Entity movement count cannot be negative");

    bool const snapMovement = mSeekTargetTick >= 0 && mSnapMovementDuringSeek;
    for (int index = 0; index < count; ++index) {
        ActorUniqueID const id{data.getVarInt64().value()};
        bool const          isPlayer = data.getBool().value();
        auto const          x        = data.getFloat().value();
        auto const          y        = data.getFloat().value();
        auto const          z        = data.getFloat().value();

        Vec3 const                              targetPosition{x, y, z};
        visuals::EntityRenderPose               previousPose{};
        visuals::EntityRenderPose               currentPose{};
        std::optional<visuals::EntityRenderKey> renderKey;
        std::shared_ptr<Packet>                 packet;
        Actor*                                  actor{};
        if (isPlayer) {
            auto const pitch    = data.getFloat().value();
            auto const yaw      = data.getFloat().value();
            auto const headYaw  = data.getFloat().value();
            auto const onGround = data.getBool().value();

            if (!mRecordedEntityIds.contains(id) || !mReplayPlayer) continue;
            actor = mReplayPlayer->getLevel().fetchEntity(id, false);
            if (!actor || !actor->isPlayer()) continue;
            // The observer is the free camera; skip its recorded movement.
            if (observerIsFree && actor == mReplayPlayer) {
                continue;
            }

            previousPose = replayRenderPose(*actor);
            renderKey    = replayEntityRenderKey(actor->getLevel(), id);
            Vec2 const rotation{pitch, yaw};
            currentPose = {
                replayRenderPosition(targetPosition),
                pitch,
                yaw,
                headYaw,
                yaw,
            };

            packet = MinecraftPackets::createPacket(MinecraftPacketIds::MovePlayer);
            if (!packet) {
                mReplayFailed = true;
                return;
            }
            auto& move          = static_cast<MovePlayerPacket&>(*packet);
            move.mPlayerID      = actor->getRuntimeID();
            move.mPos           = targetPosition;
            move.mRot           = rotation;
            move.mYHeadRot      = headYaw;
            move.mResetPosition = PlayerPositionModeComponent::PositionMode::Teleport;
            move.mTeleportData  = MovePlayerTeleportData{0, -1};
            move.mOnGround      = onGround;
        } else {
            auto const header   = data.getByte().value();
            auto const rotX     = data.getByte().value();
            auto const rotY     = data.getByte().value();
            auto const headRotY = data.getByte().value();
            auto const bodyRotY = data.getByte().value();

            if (!mRecordedEntityIds.contains(id) || !mReplayPlayer) continue;
            actor = mReplayPlayer->getLevel().fetchEntity(id, false);
            if (!actor || actor->isPlayer()) continue;

            previousPose = replayRenderPose(*actor);
            renderKey    = replayEntityRenderKey(actor->getLevel(), id);
            Vec2 const rotation{decodeRotationByte(rotX), decodeRotationByte(rotY)};
            currentPose = {
                replayRenderPosition(targetPosition),
                rotation.x,
                rotation.y,
                decodeRotationByte(headRotY),
                decodeRotationByte(bodyRotY),
            };

            packet = MinecraftPackets::createPacket(MinecraftPacketIds::MoveAbsoluteActor);
            if (!packet) {
                mReplayFailed = true;
                return;
            }
            auto& move         = *static_cast<MoveActorAbsolutePacket&>(*packet).mMoveData;
            move.mRuntimeId    = actor->getRuntimeID();
            move.mHeader->mRaw = static_cast<uchar>(header | 2);
            move.mPos          = targetPosition;
            move.mRotX         = static_cast<schar>(rotX);
            move.mRotY         = static_cast<schar>(rotY);
            move.mRotYHead     = static_cast<schar>(headRotY);
            move.mRotYBody     = static_cast<schar>(bodyRotY);
        }

        if (!dispatchMovementPacket(packet)) return;
        if (renderKey) {
            mEntityRenderKeys.insert_or_assign(id, *renderKey);
            visuals::queueReplayEntityPose(*renderKey, previousPose, currentPose);
        }
        if (actor) {
            Vec2 const rotation{currentPose.pitch, currentPose.yaw};
            pinReplayEntityHeadRotation(*actor, currentPose.headYaw, currentPose.bodyYaw);
            if (snapMovement) cancelNativeMovementInterpolation(*actor, targetPosition, rotation, currentPose.headYaw);
            else
                applyReplayEntityMovement(
                    *actor,
                    targetPosition,
                    previousPose,
                    rotation,
                    currentPose.headYaw,
                    mCurrentTick
                );
        }
    }
}

bool ReplaySession::applyPendingSnapshotLocalPlayer() {
    if (!mPendingSnapshotLocalPlayer) {
        getLogger().error("Replay snapshot {} has no pending local player to create", mReaderIndex);
        return false;
    }

    auto payload = std::move(*mPendingSnapshotLocalPlayer);
    mPendingSnapshotLocalPlayer.reset();
    if (!applyGamePacket(MinecraftPacketIds::AddPlayer, payload, true)) {
        getLogger().error("Unable to apply the CreateLocalPlayer action for replay snapshot {}", mReaderIndex);
        return false;
    }
    return true;
}

bool ReplaySession::dispatchUiPacket(std::shared_ptr<Packet> packet) {
    if (!packet || !mNetworkHandler || !packet->mHandler) return false;
    mInjectingPacket.store(packet.get(), std::memory_order_release);
    InjectionReset reset{mInjectingPacket};
    packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    return true;
}

// Container screens and server forms are only previewed for a followed, settled replay; every other case skips.
bool ReplaySession::trackScreenPacket(MinecraftPacketIds packetId, std::string_view payload) {
    if (packetId != MinecraftPacketIds::ContainerOpen && packetId != MinecraftPacketIds::ShowModalForm) {
        // ContainerClose leaves a form alone; ClientboundCloseScreen closes whichever screen is open.
        auto const closes = [&](MinecraftPacketIds opener) {
            return packetId != MinecraftPacketIds::ContainerClose || opener == MinecraftPacketIds::ContainerOpen;
        };
        for (auto it = mRecordedScreens.rbegin(); it != mRecordedScreens.rend(); ++it) {
            if (it->openTick > mCurrentTick || !closes(it->packetId)) continue;
            if (it->closeTick < 0) it->closeTick = mCurrentTick;
            break;
        }
        // A seek records every packet it crosses and decides the screen once, at the landing tick.
        if (mSeekTargetTick < 0 && mScreenPreview && closes(mScreenPreview->packetId)) {
            closeScreenPreview(packetId == MinecraftPacketIds::ContainerClose ? "recordedClose" : "closeScreen");
            popReplayScreen();
        }
        return true;
    }

    int id = 0;
    if (packetId == MinecraftPacketIds::ContainerOpen) {
        auto packet = MinecraftPackets::createPacket(packetId);
        if (!packet) return true;
        ReadOnlyBinaryStream stream(payload, false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) return true;
        id = static_cast<int>(static_cast<ContainerOpenPacket&>(*packet).mContainerId);
    }

    // Reads repeat after a backward seek, so an interval is only recorded once.
    RecordedScreen const* screen = nullptr;
    for (auto& record : mRecordedScreens) {
        if (record.closeTick < 0 && record.openTick < mCurrentTick) record.closeTick = mCurrentTick;
        if (record.packetId == packetId && record.id == id && record.openTick == mCurrentTick) screen = &record;
    }
    if (!screen) screen = &mRecordedScreens.emplace_back(packetId, id, mCurrentTick, -1, std::string(payload));

    if (mSeekTargetTick >= 0) return true;
    if (mScreenPreview && !mScreenPreview->shows(*screen)) closeScreenPreview("replaced");
    openScreenPreview(*screen);
    return true;
}

void ReplaySession::openScreenPreview(RecordedScreen const& screen) {
    mTriedScreenOpenTick = screen.openTick;
    auto packet          = MinecraftPackets::createPacket(screen.packetId);
    if (!packet) return;
    ReadOnlyBinaryStream stream(screen.payload, false);
    if (!packet->read(stream) || !stream.ensureReadCompleted()) return;

    auto* const open =
        screen.packetId == MinecraftPacketIds::ContainerOpen ? &static_cast<ContainerOpenPacket&>(*packet) : nullptr;
    int const  type      = open ? static_cast<int>(open->mType) : -1;
    auto const entityRaw = open ? open->mEntityUniqueID.get().rawID : int64{-1};
    bool const following = visuals::isReplaySubjectViewDriven() && visuals::isReplaySubjectRecorder();

    char const* action      = "inject";
    int         blockEntity = -1;
    if (!mReplayPlayer || !mReplayWorldJoined || !mNetworkHandler) action = "skip:noWorld";
    else if (!following) action = "skip:notFollowing";
    else if (mSeekTargetTick >= 0 || mIsProcessingSnapshot || mPendingSnapshotApply || mPendingReplayDimension)
        action = "skip:seeking";
    // The export waits for a menu-free scene while it initializes; finishExportTimelineInitialization reopens it.
    else if (mExportTimelinePhase == ReplayExportTimelinePhase::Initializing) action = "skip:exportInit";
    else if (mScreenPreview) action = "skip:alreadyOpen";
    else if (open) {
        if (auto const skip = containerPreviewSkip(*mReplayPlayer, *open, blockEntity)) action = skip;
    }

    getLogger().info(
        "screenPreview open action={} screen={} id={} type={} entity={} blockEntity={} following={} tick={} "
        "openTick={}",
        action,
        open ? "container" : "form",
        screen.id,
        type,
        entityRaw,
        blockEntity,
        following,
        mCurrentTick,
        screen.openTick
    );
    if (std::string_view(action) != "inject") return;

    mScreenPreview = ScreenPreview{screen.packetId, screen.id, type, screen.openTick};
    if (open) visuals::setReplayContainerKeepOpen(true);
    (void)dispatchUiPacket(std::move(packet));
}

void ReplaySession::closeScreenPreview(char const* reason) {
    if (!mScreenPreview) return;
    auto const preview   = *mScreenPreview;
    bool const container = preview.packetId == MinecraftPacketIds::ContainerOpen;
    if (container) visuals::setReplayContainerKeepOpen(false);
    getLogger().info(
        "screenPreview close reason={} screen={} id={} type={} openTick={} tick={} keepOpenOverrides={}",
        reason,
        container ? "container" : "form",
        preview.id,
        preview.type,
        preview.openTick,
        mCurrentTick,
        visuals::replayContainerKeepOpenOverrides()
    );
    if (mReplayPlayer && mReplayWorldJoined && mNetworkHandler) {
        if (!container) {
            if (auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::ClientboundCloseScreen)) {
                (void)dispatchUiPacket(std::move(packet));
            }
        } else if (auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::ContainerClose)) {
            auto& close                 = static_cast<ContainerClosePacket&>(*packet);
            close.mContainerId          = static_cast<ContainerID>(preview.id);
            close.mContainerType        = static_cast<SharedTypes::Legacy::ContainerType>(preview.type);
            close.mServerInitiatedClose = true;
            (void)dispatchUiPacket(std::move(packet));
        }
    }
    mScreenPreview.reset();
}

void ReplaySession::updateScreenPreview() {
    bool const following = visuals::isReplaySubjectViewDriven() && visuals::isReplaySubjectRecorder();
    if (!mScreenPreview) {
        // Following the recorder again shows the screen it still has open, tried once per interval.
        if (!following) mTriedScreenOpenTick = -1;
        else if (auto const* wanted = recordedScreenOpenAt(mCurrentTick);
                 wanted && wanted->openTick != mTriedScreenOpenTick && mSeekTargetTick < 0) {
            openScreenPreview(*wanted);
        }
        return;
    }
    // A native screen would keep the dimension loading screen from clearing, so it closes before the transition.
    if (!following) closeScreenPreview("followEnded");
    else if (mPendingReplayDimension) closeScreenPreview("dimension");
    if (!mScreenPreview) popReplayScreen();
}

// The landing tick has applied every action before it, so a screen opened earlier and not yet closed is the one to
// show.
ReplaySession::RecordedScreen const* ReplaySession::recordedScreenOpenAt(int tick) const {
    RecordedScreen const* wanted = nullptr;
    for (auto const& record : mRecordedScreens) {
        if (record.openTick < tick && (record.closeTick < 0 || record.closeTick >= tick)) wanted = &record;
    }
    return wanted;
}

void ReplaySession::syncScreenPreviewAfterSeek() {
    auto const* wanted = recordedScreenOpenAt(mCurrentTick);
    if (mScreenPreview && wanted && mScreenPreview->shows(*wanted)) return;
    closeScreenPreview("seek");
    if (wanted) openScreenPreview(*wanted);
    else popReplayScreen();
}

bool ReplaySession::applyGamePacket(MinecraftPacketIds packetId, std::string_view payload, bool recordedLocalPlayer) {
    if (packetId == MinecraftPacketIds::ContainerOpen || packetId == MinecraftPacketIds::ContainerClose
        || packetId == MinecraftPacketIds::ShowModalForm || packetId == MinecraftPacketIds::ClientboundCloseScreen) {
        return trackScreenPacket(packetId, payload);
    }
    // An older recording may still carry packets the recorder now ignores.
    if (describePacketLifecycle(packetId).lifecycle == PacketLifecycle::Ignore) return true;
    // The client registry is built once; the recorded table already went in at login in place of the local one.
    if (packetId == MinecraftPacketIds::ItemRegistryPacket
        && mRecordedItemRegistryApplied.load(std::memory_order_acquire)) {
        return true;
    }

    auto packet = MinecraftPackets::createPacket(packetId);
    if (!packet || !mNetworkHandler || !packet->mHandler) {
        getLogger().error("Unable to create or bind replay game packet {}", static_cast<int>(packetId));
        return false;
    }

    // MobArmorEquipmentPacket has no custom ctor initializing mSerializationMode, so the factory's
    // default (network-context dependent) can disagree with the mode the recorder used to write it.
    if (packetId == MinecraftPacketIds::MobArmorEquipment) {
        packet->setSerializationMode(SerializationMode::SemanticSideBySideLogOnMismatch);
    }

    ReadOnlyBinaryStream stream(payload, false);
    bool const           readOk    = static_cast<bool>(packet->read(stream));
    bool const           completed = readOk && static_cast<bool>(stream.ensureReadCompleted());
    if (!readOk || !completed) {
        getLogger().error("Unable to read or deserialize replay game packet {}", static_cast<int>(packetId));
        return false;
    }

    if (packetId == MinecraftPacketIds::SetTime) {
        mReplayTime = static_cast<SetTimePacket const&>(*packet).mTime;
    }

    if (packetId == MinecraftPacketIds::PlayerList) {
        auto& playerList = static_cast<PlayerListPacket&>(*packet);
        auto& entries    = *playerList.mEntries;
        // Leaving the tab list means the player left the server; the view goes back to the recorder. A player
        // who is only out of range or in another dimension keeps the choice and binds again on the next AddPlayer.
        // A camera track's subject leaving only unbinds it; the track itself still names that player.
        std::string const choice      = userReplaySubjectChoice();
        bool              removedAny  = false;
        bool              subjectLeft = false;
        for (auto& entry : entries) {
            if (auto const* removeEntry = std::get_if<PlayerListPacketPayload::RemoveEntry>(&entry)) {
                auto const uuid = removeEntry->mUUID->asString();
                if (uuid.empty()) continue;
                removedAny = true;
                if (!choice.empty() && uuid == choice) subjectLeft = true;
                forgetReplayIdentity(uuid);
                continue;
            }
            auto* addEntry = std::get_if<PlayerListPacketPayload::AddEntry>(&entry);
            if (!addEntry) continue;
            if (std::string uuid = addEntry->mUUID->asString(); !uuid.empty()) {
                bool const recorder = mRecordedLocalPlayerId && addEntry->mId->rawID == mRecordedLocalPlayerId->rawID;
                noteReplayIdentity(std::move(uuid), *addEntry->mName, ActorUniqueID{addEntry->mId->rawID}, recorder);
            }
            // Servers may withhold a player's skin; give that entry the default skin instead of dropping the player.
            if (!addEntry->mSkin->mSkinImpl) {
                getLogger().debug(
                    "Replay player-list entry '{}' has no skin data; using the default skin",
                    *addEntry->mName
                );
                *addEntry->mSkin = SerializedSkinRef::createTrustedDefaultSerializedSkin();
                if (!addEntry->mSkin->mSkinImpl) continue;
            }
            addEntry->mSkin->mSkinImpl->mObject.mIsPrimaryUser = false;
        }
        if (subjectLeft) {
            getLogger().debug("replay subject left uuid={} tick={} fallback=recorder", choice, mCurrentTick);
            setReplaySubjectChoice({});
        } else if (removedAny) {
            rebindReplaySubject();
        }
    }

    if (packetId == MinecraftPacketIds::PlayerSkin) {
        // Same rule as the player list: a recorded skin must never be treated as the viewer's own.
        auto& skinImpl = static_cast<PlayerSkinPacket&>(*packet).mSkin->mSkinImpl;
        if (!skinImpl) {
            getLogger().debug("Skipping a replay player skin packet without skin data");
            return true;
        }
        skinImpl->mObject.mIsPrimaryUser = false;
    }

    if (packetId == MinecraftPacketIds::ChangeDimension) {
        if (!mReplayPlayer) return false;
        auto const&        change   = static_cast<ChangeDimensionPacket const&>(*packet);
        auto const&        position = *change.mPos;
        auto const&        rotation = mReplayPlayer->getRotation();
        PlaybackView const view{position.x, position.y, position.z, rotation.y, rotation.x};
        (void)ensureReplayDimension(*change.mDimensionId, view);
        return !mReplayFailed;
    }

    ActorUniqueID  entityId{};
    ActorRuntimeID runtimeId{};
    Vec3           entityPosition{};
    bool           addsEntity = true;
    switch (packetId) {
    case MinecraftPacketIds::AddActor: {
        auto const& addActor = static_cast<AddActorPacket const&>(*packet);
        entityId             = *addActor.mEntityId;
        runtimeId            = *addActor.mRuntimeId;
        entityPosition       = *addActor.mPos;
        break;
    }
    case MinecraftPacketIds::AddItemActor: {
        auto const& addItem = static_cast<AddItemActorPacket const&>(*packet);
        entityId            = *addItem.mId;
        runtimeId           = *addItem.mRuntimeId;
        entityPosition      = *addItem.mPos;
        break;
    }
    case MinecraftPacketIds::AddPainting: {
        auto const& addPainting = static_cast<AddPaintingPacket const&>(*packet);
        entityId                = *addPainting.mEntityId;
        runtimeId               = *addPainting.mRuntimeId;
        entityPosition          = *addPainting.mPos;
        break;
    }
    case MinecraftPacketIds::AddPlayer: {
        auto& addPlayer = static_cast<AddPlayerPacket&>(*packet);
        entityPosition  = *addPlayer.mPos;
        // Older CreateLocalPlayer snapshots retained the recording world's live player ID.
        std::optional<ActorUniqueID> sourceId;
        if (recordedLocalPlayer) {
            sourceId = *addPlayer.mAbilitiesData->mTargetPlayer;
        } else if (auto client = ll::service::getClientInstance(); client && client->getLocalPlayer()) {
            sourceId = client->getLocalPlayer()->getOrCreateUniqueID();
        }
        if (sourceId) {
            if (addPlayer.mAbilitiesData->mTargetPlayer->rawID == sourceId->rawID) {
                addPlayer.mAbilitiesData->mTargetPlayer = RecordedPlayerUniqueId;
            }
            for (auto& link : *addPlayer.mLinks) {
                if (link.A->rawID == sourceId->rawID) link.A = RecordedPlayerUniqueId;
                if (link.B->rawID == sourceId->rawID) link.B = RecordedPlayerUniqueId;
            }
        }
        if (recordedLocalPlayer && mReplayPlayer && *addPlayer.mName == "__playback_" + addPlayer.mUuid->asString()) {
            // Older snapshots kept the original name in the player list under the synthetic UUID.
            auto const& entries = mReplayPlayer->getLevel().getPlayerList();
            auto const  entry   = entries.find(*addPlayer.mUuid);
            if (entry != entries.end() && !entry->second.mName->empty()) {
                *addPlayer.mName = *entry->second.mName;
            }
        }
        if (addPlayer.mPlayerGameType == GameType::Default || addPlayer.mPlayerGameType == GameType::Undefined) {
            LayeredAbilities abilities;
            addPlayer.mAbilitiesData->fillIn(abilities);
            if (abilities.getBool(AbilitiesIndex::NoClip)) {
                addPlayer.mPlayerGameType = GameType::Spectator;
            } else if (abilities.getBool(AbilitiesIndex::Instabuild)) {
                addPlayer.mPlayerGameType = GameType::Creative;
            } else if (abilities.getBool(AbilitiesIndex::Build) || abilities.getBool(AbilitiesIndex::Mine)) {
                addPlayer.mPlayerGameType = GameType::Survival;
            } else {
                addPlayer.mPlayerGameType = GameType::Adventure;
            }
        }
        // 26.40 carries the player's unique ID inside the abilities payload.
        entityId  = *addPlayer.mAbilitiesData->mTargetPlayer;
        runtimeId = *addPlayer.mRuntimeId;
        break;
    }
    default:
        addsEntity = false;
        break;
    }

    if (addsEntity && mReplayPlayer) {
        auto& level            = mReplayPlayer->getLevel();
        auto* uniqueCollision  = level.fetchEntity(entityId, false);
        auto* runtimeCollision = level.getRuntimeEntity(runtimeId, false);
        if (uniqueCollision || runtimeCollision) {
            getLogger().warn(
                "Skipping conflicting replay entity packet {} (unique={}, runtime={})",
                packet->getName(),
                entityId.rawID,
                runtimeId.rawID
            );
            return true;
        }

        // Recorded IDs never raise the runtime-id allocator: that made native code drop the real local player.
    }

    if (mSeekChunksDeferred && !mIsProcessingSnapshot) {
        if (holdSeekBlockPacket(*packet, payload)) return true;
        if (addsEntity) {
            // An entity added to a column the client has not loaded may never be placed, so its chunk lands first.
            ChunkPos const column{
                static_cast<int>(std::floor(entityPosition.x)) >> 4,
                static_cast<int>(std::floor(entityPosition.z)) >> 4
            };
            auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
            if (replayDimension && mSeekLatestLevelChunks.contains(column)) {
                auto chunk = replayDimension->mChunkSource->getExistingChunk(column);
                if (!chunk || chunk->mIsEmptyClientChunk
                    || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
                    flushSeekChunks();
                    if (mReplayFailed) return false;
                    mSeekReleasedPackets.push_back(
                        SeekReleasedPacket{packetId, std::string(payload), recordedLocalPlayer}
                    );
                    mSeekTickInterrupted = mChunkInjectionPending;
                    if (!mChunkInjectionPending) return releaseSeekPackets();
                    return true;
                }
            }
        }
    }

    // Must also run while chunks are streaming: a seek keeps applying block updates between batches.
    if (!mIsProcessingSnapshot) invalidateSnapshotColumns(*packet);

    // Texture updates do not create the map. A recording that started after the map was created never carries the
    // creation packet, so the level has to own the map before the native handler will keep its pixels.
    if (packetId == MinecraftPacketIds::MapData && mReplayPlayer) {
        auto& mapPacket = static_cast<ClientboundMapItemDataPacket&>(*packet);
        if (!mapPacket.mMapPixels->empty()) {
            auto&                      level = mReplayPlayer->getLevel();
            std::vector<ActorUniqueID> ids{*mapPacket.mMapId};
            ids.insert(ids.end(), mapPacket.mCreationMapIds->begin(), mapPacket.mCreationMapIds->end());
            for (auto const& id : ids) {
                if (level.getMapSavedData(id)) continue;
                (void)level.createMapSavedData(
                    id,
                    mapPacket.mMapOrigin,
                    DimensionType{static_cast<int>(mapPacket.mDimension)},
                    static_cast<int>(mapPacket.mScale)
                );
            }
        }
    }

    mInjectingPacket.store(packet.get(), std::memory_order_release);
    InjectionReset reset{mInjectingPacket};

    // The recorded local player's inventory packets land on the observer, which is what the HUD draws. While another
    // player's HUD is shown they would overwrite it, so they update the saved slots the restore puts back instead.
    if (mFollowedHudMirror && mFollowedHudMirror->active && divertRecordedHudPacket(*packet, *mFollowedHudMirror)) {
        return true;
    }

    packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);

    if (mReplayPlayer) {
        std::string const subject = [&]() -> std::string {
            std::string const choice = replaySubjectChoice();
            if (!choice.empty()) return choice;
            if (!mRecordedLocalPlayerId) return {};
            std::lock_guard lock(mReplayPlayersMutex);
            for (auto const& [uuid, record] : mReplayPlayers) {
                if (record.id == *mRecordedLocalPlayerId) return uuid;
            }
            return {};
        }();
        if (!subject.empty() && packetId == MinecraftPacketIds::PlayerSkin) {
            auto const& skinPacket = static_cast<PlayerSkinPacket const&>(*packet);
            if (skinPacket.mUUID->asString() == subject && skinPacket.mSkin->mSkinImpl) {
                applyObserverSkin(*skinPacket.mSkin);
                getLogger().debug("replay observer skin refreshed uuid={}", subject);
            }
        } else if (!subject.empty() && packetId == MinecraftPacketIds::PlayerList) {
            auto const& playerList = static_cast<PlayerListPacket const&>(*packet);
            for (auto const& entry : *playerList.mEntries) {
                auto const* addEntry = std::get_if<PlayerListPacketPayload::AddEntry>(&entry);
                if (!addEntry || addEntry->mUUID->asString() != subject || !addEntry->mSkin->mSkinImpl) continue;
                applyObserverSkin(*addEntry->mSkin);
                getLogger().debug("replay observer skin refreshed uuid={}", subject);
                break;
            }
        }
    }

    if (packetId == MinecraftPacketIds::MapData && mReplayPlayer) {
        static auto const applyToMap =
            runtime::gameMember<void (ClientboundMapItemDataPacketPayload::*)(MapItemSavedData&, bool) const>(
                runtime::GameFunction::MapDataApplyToMap
            );
        auto& map   = static_cast<ClientboundMapItemDataPacket&>(*packet);
        auto& level = mReplayPlayer->getLevel();
        if (!map.mMapPixels->empty()) {
            std::vector<ActorUniqueID> ids{*map.mMapId};
            ids.insert(ids.end(), map.mCreationMapIds->begin(), map.mCreationMapIds->end());
            for (auto const& id : ids) {
                auto* saved = level.getMapSavedData(id);
                if (!saved) {
                    saved = &level.createMapSavedData(
                        id,
                        map.mMapOrigin,
                        DimensionType{static_cast<int>(map.mDimension)},
                        static_cast<int>(map.mScale)
                    );
                }
                if (saved->mPixels->empty()) (map.*applyToMap)(*saved, false);
            }
        }
    }

    if (addsEntity) mRecordedEntityIds.emplace(entityId);

    if (packetId == MinecraftPacketIds::AddPlayer && mReplayPlayer) {
        auto const& addPlayer = static_cast<AddPlayerPacket const&>(*packet);
        if (recordedLocalPlayer) mRecordedLocalPlayerId = entityId;
        std::string uuid = addPlayer.mUuid->asString();
        std::string name = *addPlayer.mName;
        if (!uuid.empty()) rememberReplayPlayer(std::move(uuid), std::move(name), entityId, recordedLocalPlayer);
        // A player who never moves again has no movement action to seed the pose history a first-person view reads.
        auto& level = mReplayPlayer->getLevel();
        if (auto* actor = level.fetchEntity(entityId, false)) {
            if (actor->isPlayer()) {
                auto const& entries = level.getPlayerList();
                auto const  entry   = entries.find(*addPlayer.mUuid);
                if (entry != entries.end() && entry->second.mSkin->mSkinImpl) {
                    auto& player = static_cast<Player&>(*actor);
                    player.updateSkin(*entry->second.mSkin, static_cast<int>(player.getClientSubId()));
                    getLogger()
                        .debug("replay skin applied name={} uuid={}", *addPlayer.mName, addPlayer.mUuid->asString());
                }
            }
            if (auto const renderKey = replayEntityRenderKey(level, entityId)) {
                auto const pose = replayRenderPose(*actor);
                mEntityRenderKeys.insert_or_assign(entityId, *renderKey);
                visuals::queueReplayEntityPose(*renderKey, pose, pose);
            }
        }
        rebindReplaySubject();
    }

    switch (packetId) {
    case MinecraftPacketIds::RemoveActor: {
        auto const id = *static_cast<RemoveActorPacket const&>(*packet).mEntityId;
        mRecordedEntityIds.erase(id);
        std::optional<visuals::EntityRenderKey> removedKey;
        if (auto const renderKey = mEntityRenderKeys.find(id); renderKey != mEntityRenderKeys.end()) {
            removedKey = renderKey->second;
            visuals::removeReplayEntityPose(renderKey->second);
            mEntityRenderKeys.erase(renderKey);
        }
        gReplayHandBobTick.erase(id.rawID);
        markReplayPlayerAbsent(id);
        if (removedKey && visuals::isReplaySubject(*removedKey)) visuals::setReplaySubject(std::nullopt);
        rebindReplaySubject();
        break;
    }
    case MinecraftPacketIds::SetDisplayObjective: {
        auto const& objectiveName = *static_cast<SetDisplayObjectivePacket const&>(*packet).mObjectiveName;
        if (!objectiveName.empty()) mReplayObjectiveNames.emplace(objectiveName);
        break;
    }
    case MinecraftPacketIds::RemoveObjective:
        mReplayObjectiveNames.erase(*static_cast<RemoveObjectivePacket const&>(*packet).mObjectiveName);
        break;
    case MinecraftPacketIds::PlayerEquipment: {
        auto const& equipment = static_cast<MobEquipmentPacket const&>(*packet);
        if (equipment.mContainerId != ContainerID::Inventory) break;
        if (equipment.mRuntimeId->rawID == record::RecordedPlayerRuntimeIdValue) break;
        auto const level = ll::service::getMultiPlayerLevel();
        if (!level) break;
        int const slot = equipment.mSlot;
        if (slot < 0 || slot >= 9) break;
        auto& hotbar = gReplayHotbars[equipment.mRuntimeId->rawID];
        hotbar.items[static_cast<size_t>(slot)] = itemFromDescriptor(*equipment.mItem, level->getBlockPalette());
        hotbar.selected = std::clamp(static_cast<int>(equipment.mSelectedSlot), 0, 8);
        break;
    }
    case MinecraftPacketIds::UpdateAttributes: {
        auto& attributes = static_cast<UpdateAttributesPacket&>(*packet);
        if (mReplayPlayer && attributes.mRuntimeId->rawID == record::RecordedPlayerRuntimeIdValue) {
            if (mFollowedHudMirror && mFollowedHudMirror->active) {
                keepRecordedAttributes(*mFollowedHudMirror, attributes);
            } else {
                mirrorRecordedAttributes(*mReplayPlayer, attributes);
            }
        }
        break;
    }
    case MinecraftPacketIds::UpdatePlayerGameType: {
        auto const& gameType = static_cast<UpdatePlayerGameTypePacket const&>(*packet);
        if (gameType.mTargetPlayer->rawID == record::RecordedPlayerUniqueIdValue) {
            visuals::setReplayHudGameType(static_cast<int>(gameType.mPlayerGameType));
        }
        break;
    }
    default:
        break;
    }
    return true;
}

void ReplaySession::invalidateSnapshotColumns(Packet const& packet) {
    auto invalidate = [this](BlockPos const& pos) { mDirtySnapshotColumns.emplace(ChunkPos{pos}); };

    switch (packet.getId()) {
    case MinecraftPacketIds::UpdateBlock:
        invalidate(*static_cast<UpdateBlockPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::UpdateBlockSynced:
        invalidate(*static_cast<UpdateBlockSyncedPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::UpdateSubChunkBlocks: {
        auto const& subChunk = static_cast<UpdateSubChunkBlocksPacket const&>(packet);
        auto const& changed  = *subChunk.mBlocksChanged;
        invalidate(*subChunk.mSubChunkBlockPosition);
        for (auto const* blocks : {&*changed.mStandards, &*changed.mExtras}) {
            for (auto const& info : *blocks) invalidate(*info.mPos);
        }
        break;
    }
    case MinecraftPacketIds::BlockActorData:
        invalidate(*static_cast<BlockActorDataPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::TileEvent: {
        auto const& pos    = *static_cast<BlockEventPacket const&>(packet).mPos;
        auto const  column = ChunkPos{pos};
        mDirtySnapshotColumns.emplace(column);
        mDirtySnapshotColumns.emplace(column.x - 1, column.z);
        mDirtySnapshotColumns.emplace(column.x + 1, column.z);
        mDirtySnapshotColumns.emplace(column.x, column.z - 1);
        mDirtySnapshotColumns.emplace(column.x, column.z + 1);
        break;
    }
    default:
        break;
    }
}

bool ReplaySession::flushPendingSnapshotGamePackets(
    bool                                         playerListOnly,
    size_t                                       maxPackets,
    std::chrono::steady_clock::time_point const& deadline
) {
    auto pending = std::move(mPendingSnapshotGamePackets);
    mPendingSnapshotGamePackets.clear();

    size_t applied = 0;
    for (auto& [packetId, payload] : pending) {
        if (playerListOnly != (packetId == MinecraftPacketIds::PlayerList)) {
            mPendingSnapshotGamePackets.emplace_back(packetId, std::move(payload));
            continue;
        }
        if (applied >= maxPackets || (applied != 0 && std::chrono::steady_clock::now() >= deadline)) {
            mPendingSnapshotGamePackets.emplace_back(packetId, std::move(payload));
            continue;
        }

        if (!applyGamePacket(packetId, payload)) {
            getLogger().error(
                "Unable to apply replay snapshot game packet {} (playerListOnly={})",
                static_cast<int>(packetId),
                playerListOnly
            );
            return false;
        }
        ++applied;
    }
    if (applied != 0) {
        getLogger().debug(
            "Applied {} replay snapshot game packets in {} phase",
            applied,
            playerListOnly ? "player-list" : "entity"
        );
    }
    return true;
}

bool ReplaySession::clearRecordedEntities() {
    visuals::setReplaySubject(std::nullopt);
    clearReplayPlayers(false);
    gReplayHandBobTick.clear();
    gReplayHotbars.clear();
    mEntityRenderKeys.clear();
    visuals::clearReplayEntityPoses();
    if (mRecordedEntityIds.empty()) return true;
    if (!mNetworkHandler) return false;

    auto ids = std::move(mRecordedEntityIds);
    mRecordedEntityIds.clear();
    mHeldItemUse.clear();
    for (auto const& id : ids) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::RemoveActor);
        if (!packet || !packet->mHandler) return false;
        static_cast<RemoveActorPacket&>(*packet).mEntityId = id;

        mInjectingPacket.store(packet.get(), std::memory_order_release);
        InjectionReset reset{mInjectingPacket};
        packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    }
    return true;
}

bool ReplaySession::applyRequestModeLevelChunkDirect(std::string_view payload) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
    if (!packet) return false;

    ReadOnlyBinaryStream packetStream(payload, false);
    if (!packet->read(packetStream) || !packetStream.ensureReadCompleted()) {
        return false;
    }

    auto const& levelChunk = static_cast<LevelChunkPacket const&>(*packet);
    if (!applyRequestModeLevelChunkData(levelChunk)) return false;

    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mCompletedLevelChunkPositions.emplace(*levelChunk.mPos);
    }
    mChunkCompletionObserved.store(true, std::memory_order_release);
    ++mDirectLevelChunks;
    return true;
}

bool ReplaySession::applyRequestModeLevelChunkData(LevelChunkPacket const& levelChunk) {
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    // 26.40 folded the request-mode flag into the limit: a value means the client must request subchunks.
    if (static_cast<bool>(levelChunk.mCacheEnabled) || !levelChunk.mClientRequestSubChunkLimit->has_value()
        || static_cast<DimensionType const&>(levelChunk.mDimensionId) != replayDimension->getDimensionId()) {
        return false;
    }

    auto const& pos   = *levelChunk.mPos;
    auto        chunk = replayDimension->mChunkSource->getExistingChunk(pos);
    if (!chunk || chunk->mIsEmptyClientChunk
        || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
        return false;
    }

    try {
        ReadOnlyBinaryStream levelStream(*levelChunk.mSerializedChunk, false);
        VarIntDataInput      levelInput(levelStream);
        chunk->deserializeBiomes(levelInput, true);
        chunk->deserializeBorderBlocks(levelInput);
        if (!levelStream.ensureReadCompleted()) return false;
    } catch (std::exception const& exception) {
        getLogger()
            .error("Unable to apply replay LevelChunk data directly at ({}, {}): {}", pos.x, pos.z, exception.what());
        return false;
    } catch (...) {
        getLogger().error("Unable to apply replay LevelChunk data directly at ({}, {})", pos.x, pos.z);
        return false;
    }

    return true;
}

bool ReplaySession::applySubChunkDirect(std::string_view payload) {
    if (!mNetworkHandler) return false;
    if (!refreshReplayPlayer()) return false;
    auto*       localPlayer     = static_cast<LocalPlayer*>(mReplayPlayer);
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
    if (!packet) return false;

    ReadOnlyBinaryStream stream(payload, false);
    if (!packet->read(stream) || !stream.ensureReadCompleted()) return false;

    auto const& subChunk = static_cast<SubChunkPacket const&>(*packet);
    if (static_cast<bool>(subChunk.mCacheEnabled)
        || static_cast<DimensionType const&>(subChunk.mDimensionType) != replayDimension->getDimensionId()) {
        return false;
    }

    size_t appliedEntries = 0;
    try {
        localPlayer->getLevel().notifySubChunkRequestManager(subChunk);
        for (auto const& entry : *subChunk.mSubChunkData) {
            if (!isSuccessfulSubChunkResult(static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult))) {
                continue;
            }
            mNetworkHandler
                ->_handleSubChunkData(mNetworkHandler->mServerGuid.get(), subChunk, entry, localPlayer, true);
            ++appliedEntries;
        }
    } catch (std::exception const& exception) {
        getLogger().error("Unable to apply replay SubChunk data directly: {}", exception.what());
        return false;
    } catch (...) {
        getLogger().error("Unable to apply replay SubChunk data directly");
        return false;
    }

    ++mDirectSubChunkPackets;
    mDirectSubChunkEntries += appliedEntries;
    return true;
}

bool ReplaySession::injectChunkPacket(std::string_view payload, MinecraftPacketIds packetId) {
    if (!mNetworkHandler) return false;
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    auto packet = MinecraftPackets::createPacket(packetId);
    if (!packet) return false;

    ReadOnlyBinaryStream stream(payload, false);
    if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) return false;

    size_t subChunkEntries = 0;
    if (packetId == MinecraftPacketIds::FullChunkData) {
        auto& levelChunk = static_cast<LevelChunkPacket&>(*packet);
        if (static_cast<bool>(levelChunk.mCacheEnabled)) {
            return false;
        }
        auto const packetDimension = static_cast<DimensionType const&>(levelChunk.mDimensionId);
        if (packetDimension != replayDimension->getDimensionId()) {
            noteSkippedReplayChunk(
                true,
                packetDimension.mValue,
                replayDimension->getDimensionId().mValue,
                mCurrentTick
            );
            return true;
        }

        auto const& pos = *levelChunk.mPos;
        if (mApplyingChunkSnapshot) mApplyingSnapshotChunks.emplace(pos);
        else mSnapshotChunks.emplace(pos);
        auto& chunkSource = *replayDimension->mChunkSource;
        auto  replayChunk = chunkSource.getExistingChunk(pos);
        if (!replayChunk) {
            replayChunk = chunkSource.getOrLoadChunk(pos, ChunkSource::LoadMode::None, false);
        }
        if (!replayChunk) {
            getLogger()
                .error("Unable to create replay-world LevelChunk at tick {} for ({}, {})", mCurrentTick, pos.x, pos.z);
            return false;
        }
        {
            std::scoped_lock lock(mPendingLevelChunksMutex);
            mRetainedReplayChunks.insert_or_assign(pos, replayChunk);
            mPendingLevelChunks.emplace(pos);
        }
        if (levelChunk.mClientRequestSubChunkLimit->has_value() && !replayChunk->mIsEmptyClientChunk
            && replayChunk->mLoadState->load(std::memory_order_acquire) == ChunkState::Loaded
            && replayChunk->mClientNeedsToRequestSubChunks) {
            // Recorded columns must replace old data rather than enter the native re-request-only branch.
            replayChunk->mClientNeedsToRequestSubChunks = false;
        }
        mChunkInjectionPending = true;
    } else if (packetId == MinecraftPacketIds::SubChunkPacket) {
        auto& subChunk = static_cast<SubChunkPacket&>(*packet);
        if (static_cast<bool>(subChunk.mCacheEnabled)) {
            return false;
        }
        auto const packetDimension = static_cast<DimensionType const&>(subChunk.mDimensionType);
        if (packetDimension != replayDimension->getDimensionId()) {
            noteSkippedReplayChunk(
                false,
                packetDimension.mValue,
                replayDimension->getDimensionId().mValue,
                mCurrentTick
            );
            return true;
        }
        auto& entries      = *subChunk.mSubChunkData;
        auto  isSuccessful = [](SubChunkPacket::SubChunkPacketData const& entry) {
            return isSuccessfulSubChunkResult(static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult));
        };
        std::erase_if(entries, [&isSuccessful](auto const& entry) { return !isSuccessful(entry); });
        subChunkEntries = entries.size();
        if (subChunkEntries == 0) return true;

    } else {
        return false;
    }

    mInjectingPacket.store(packet.get(), std::memory_order_release);
    InjectionReset reset{mInjectingPacket};
    packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    if (packetId == MinecraftPacketIds::FullChunkData) {
        ++mInjectedLevelChunks;
    } else {
        ++mInjectedSubChunkPackets;
        mInjectedSubChunkEntries += subChunkEntries;
    }
    return true;
}

bool ReplaySession::clearReplayObjectives() {
    if (mReplayObjectiveNames.empty()) return true;
    if (!mNetworkHandler) return false;

    auto objectiveNames = std::move(mReplayObjectiveNames);
    mReplayObjectiveNames.clear();
    for (auto& objectiveName : objectiveNames) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::RemoveObjective);
        if (!packet || !packet->mHandler) return false;
        static_cast<RemoveObjectivePacket&>(*packet).mObjectiveName = objectiveName;

        mInjectingPacket.store(packet.get(), std::memory_order_release);
        InjectionReset reset{mInjectingPacket};
        packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    }
    return true;
}

bool ReplaySession::isReplayLevel(Level const& level) {
    auto const& session = getInstance();
    return (session.mActive || session.mCleanupState != CleanupState::None) && !session.mReplayLevelId.empty()
        && level.getLevelId() == session.mReplayLevelId;
}

bool ReplaySession::isReplayWorldLevelId(std::string_view levelId) const {
    auto const profile = mReplayDimensionProfile.load(std::memory_order_acquire);
    return profile && levelId == profile->levelId;
}

void ReplaySession::configureReplayDimension(DimensionArguments& arguments) const {
    auto profile = mReplayDimensionProfile.load(std::memory_order_acquire);
    if (!profile || arguments.mDerived->mLevel.getLevelId() != profile->levelId) return;

    auto const dimensionId = arguments.mDimId->mValue;
    if (auto const name = profile->names.find(dimensionId); name != profile->names.end()) {
        arguments.mName = name->second;
    }

    auto const range = profile->heightRanges.find(dimensionId);
    if (range == profile->heightRanges.end()) return;

    arguments.mHeightRange->mMin = static_cast<short>(range->second.minimum);
    arguments.mHeightRange->mMax = static_cast<short>(range->second.maximum);
}

bool ReplaySession::shouldIsolateChunkPackets() const {
    if (!mActive || mReplayLevelId.empty()) return false;

    auto level = ll::service::getMultiPlayerLevel();
    return level && level->getLevelId() == mReplayLevelId;
}

bool ReplaySession::shouldSuppressNativeChunk(ChunkPos const& pos, DimensionType packetDimension) const {
    // Snapshot column coordinates are only meaningful in their owning dimension.
    if (mPendingReplayDimension && *mPendingReplayDimension == packetDimension) return false;
    if (mChunkIsolationDimension && *mChunkIsolationDimension != packetDimension) return false;

    if (!mChunkInjectionPlanPrepared && (!mWorldReady || mApplyingChunkSnapshot)) return true;

    return mSnapshotChunks.contains(pos) || mApplyingSnapshotChunks.contains(pos);
}

void ReplaySession::setMinecraftScreenModel(std::shared_ptr<MinecraftScreenModel> const& screenModel) {
    mScreenModel = screenModel;
}

void ReplaySession::onLevelJoined(Player& player) {
    if (!mActive) return;
    if (!isReplayLevel(player.getLevel())) {
        getLogger().error(
            "The replay world did not open; replay cancelled (joinedLevelId={}, expectedLevelId={})",
            player.getLevel().getLevelId(),
            mReplayLevelId
        );
        getLogger().flush();
        stop();
        return;
    }

    mReplayWorldJoined = true;
    mReplayPlayer      = &player;
    if (!refreshReplayPlayer()) {
        getLogger().error("Replay player is unavailable immediately after joining the replay world");
    }
}

void ReplaySession::onLevelStartJoin() {
    mScreenModel.reset();
    if (mActive && mReplayWorldJoined) {
        stop();
        return;
    }
    clearNetworkContext();
}

void ReplaySession::onLevelExit() {
    if (mReplayLevelId.empty()) return;
    if (mCleanupState == CleanupState::None) {
        if (!mActive || !mReplayWorldJoined) return;
        mCleanupState = CleanupState::ReadyToDelete;
    } else if (mCleanupState == CleanupState::WaitingForExit) {
        mCleanupState = CleanupState::ReadyToDelete;
    } else {
        return;
    }

    clearReplayData();
    mReplayWorldJoined = false;
    mCleanupWaitTicks  = 0;
}

void ReplaySession::onLevelJoinCancelled() {
    if (mReplayLevelId.empty() || mReplayWorldJoined) return;
    if (mCleanupState == CleanupState::None) {
        if (!mActive) return;
        mCleanupState = CleanupState::ReadyToDelete;
    } else if (mCleanupState == CleanupState::WaitingForExit) {
        mCleanupState = CleanupState::ReadyToDelete;
    } else {
        return;
    }

    clearReplayData();
    mCleanupWaitTicks = 0;
}

void ReplaySession::tryFinalizeWorldCleanup() {
    auto client = ll::service::getClientInstance();
    if (!client || !client->isLeaveGameDone() || client->hasLevel() || client->isWorldActive()) return;

    auto& game = client->getMinecraftGame_DEPRECATED();
    if (game.isInServer() || game.getServerInstance()) return;

    auto& cache          = game.getLevelListCache();
    auto  basePathBuffer = cache.getBasePath();
    auto  worldsPath     = std::filesystem::path(basePathBuffer.get());

    if (mCleanupState == CleanupState::None) {
        if (mActive || !mReplayLevelId.empty() || mOrphanReplayWorldsScanned) return;

        std::error_code ec;
        auto            entries = std::filesystem::directory_iterator(worldsPath, ec);
        if (ec) {
            if (mCleanupWaitTicks++ == 0) {
                getLogger().error("Unable to scan for orphaned replay worlds: {}", ec.message());
            }
            return;
        }

        for (auto const& entry : entries) {
            if (!entry.is_directory(ec)) {
                if (ec) break;
                continue;
            }

            auto levelId = entry.path().filename().string();
            if (!isValidReplayLevelId(levelId)) continue;

            getLogger().debug("Found orphaned replay world {}; scheduling removal", levelId);
            mReplayLevelId    = std::move(levelId);
            mCleanupState     = CleanupState::ReadyToDelete;
            mCleanupWaitTicks = 0;
            break;
        }
        if (ec) {
            getLogger().error("Unable to finish scanning for orphaned replay worlds: {}", ec.message());
            return;
        }
        if (mCleanupState == CleanupState::None) {
            mOrphanReplayWorldsScanned = true;
            mCleanupWaitTicks          = 0;
            return;
        }
    }

    if (mCleanupState == CleanupState::WaitingForExit) {
        mCleanupState      = CleanupState::ReadyToDelete;
        mReplayWorldJoined = false;
        mCleanupWaitTicks  = 0;
    }

    if (!isValidReplayLevelId(mReplayLevelId)) {
        if (mCleanupWaitTicks++ == 0) {
            getLogger().error("Refusing to remove invalid replay world id {}", mReplayLevelId);
        }
        return;
    }

    auto worldPath = worldsPath / mReplayLevelId;

    std::error_code ec;
    bool            worldFilesExist = std::filesystem::exists(worldPath, ec);
    if (ec) {
        if (mCleanupWaitTicks++ == 0) {
            getLogger().error("Unable to inspect replay world {}: {}", mReplayLevelId, ec.message());
        }
        return;
    }

    bool levelIsCached = cache.hasLevelWithId(mReplayLevelId);
    if (!levelIsCached && !worldFilesExist) {
        getLogger().debug("Removed replay world {}", mReplayLevelId);
        finishWorldCleanup();
        return;
    }

    if (mCleanupState == CleanupState::ReadyToDelete) {
        getLogger().debug("Removing replay world {}", mReplayLevelId);
        mCleanupState     = CleanupState::DeleteIssued;
        mCleanupWaitTicks = 0;
        try {
            cache.deleteLevel(mReplayLevelId);
        } catch (std::exception const& e) {
            getLogger().error("Unable to remove replay world {}: {}", mReplayLevelId, e.what());
        } catch (...) {
            getLogger().error("Unable to remove replay world {}", mReplayLevelId);
        }
        return;
    }

    ++mCleanupWaitTicks;
    if (mCleanupWaitTicks == REPLAY_WORLD_DELETE_TIMEOUT_TICKS) {
        getLogger().error("Replay world {} still exists after deletion was requested", mReplayLevelId);
    }
}

void ReplaySession::captureNetworkContext(LegacyClientNetworkHandler& handler) {
    if (mNetworkHandler == &handler) return;

    mNetworkHandler = &handler;
}

void ReplaySession::clearNetworkContext() { mNetworkHandler = nullptr; }

void ReplaySession::onLevelChunkHandled(ChunkPos const& pos, Dimension const& dimension) {
    if (mReplayDimension.load(std::memory_order_acquire) != &dimension) return;

    auto       chunk      = dimension.mChunkSource->getExistingChunk(pos);
    auto const loadState  = chunk ? chunk->mLoadState->load(std::memory_order_acquire) : ChunkState::Unloaded;
    bool const retainable = chunk && !chunk->mIsEmptyClientChunk && loadState == ChunkState::Loaded;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        auto             it = mPendingLevelChunks.find(pos);
        if (it == mPendingLevelChunks.end()) return;

        mPendingLevelChunks.erase(it);
        mCompletedLevelChunkPositions.emplace(pos);
        if (retainable) mRetainedReplayChunks.insert_or_assign(pos, chunk);
        else mRetainedReplayChunks.erase(pos);
        mChunkCompletionObserved.store(true, std::memory_order_release);
    }

    if (!retainable) {
        getLogger().warn(
            "Replay LevelChunk completion for ({}, {}) did not leave a loaded chunk in the replay ChunkSource "
            "(existing={}, empty={}, load_state={})",
            pos.x,
            pos.z,
            static_cast<bool>(chunk),
            chunk ? static_cast<bool>(chunk->mIsEmptyClientChunk) : false,
            chunk ? static_cast<int>(loadState) : -1
        );
    }
}

} // namespace playback::replay
