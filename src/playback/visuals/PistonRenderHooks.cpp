#include "PistonRenderHooks.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/blockactor/BlockActorRenderData.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/blockactor/PistonBlockActorRenderer.h"
#include "mc/world/actor/ActorTerrainInterlockData.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/MovingBlockActor.h"
#include "mc/world/level/block/actor/PistonBlockActor.h"
#include "mc/world/level/block/actor/PistonState.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace playback::visuals {

namespace {

using VisibilityState = ::ActorTerrainInterlockData::VisibilityState;

std::atomic_bool gInstalled{false};

bool smoothPistonRenderEnabled() { return Playback::getInstance().getConfig().smoothPistonRender; }

struct BlockPosHash {
    [[nodiscard]] size_t operator()(::BlockPos const& pos) const noexcept {
        return (static_cast<size_t>(static_cast<uint32_t>(pos.x)) * 73856093u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.y)) * 19349663u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.z)) * 83492791u);
    }
};

// Tick at which a MovingBlock was seen detached from its cell; the interlock struct has no field to reuse for this.
std::unordered_map<::MovingBlockActor const*, uint64_t> gTailAnchors;

// Replay rebuilds PistonBlockActors without retiring the old ones, leaving several on one cell that the engine draws a
// head for each. Animating ones advance mProgress in $tick; leftovers never do, so stepping distinguishes the two.
std::mutex                              gSteppedPistonMutex;
std::unordered_set<::BlockActor const*> gSteppedPistons;

// Ownership is scoped to one round: a destroyed piston's address cannot be detected, and a claim outliving its owner
// locked the cell for good, leaving frames with no head at all. Re-electing each round bounds a stale address to the
// round it was seen in. The round advances on PistonBlockActor::$tick, which runs once per game tick.
std::atomic<uint64_t> gHeadOwnerRound{0};

struct HeadOwner {
    uint64_t            round{};
    ::BlockActor const* actor{};
    int                 rank{};
};

std::mutex                                              gHeadOwnerMutex;
std::unordered_map<::BlockPos, HeadOwner, BlockPosHash> gHeadOwners;

void markPistonStepped(::BlockActor const* piston) {
    std::lock_guard const guard(gSteppedPistonMutex);
    gSteppedPistons.insert(piston);
}

bool hasPistonStepped(::BlockActor const* piston) {
    std::lock_guard const guard(gSteppedPistonMutex);
    return gSteppedPistons.contains(piston);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonArmVisibilityHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$ctor,
    void*,
    ::BlockPos const& pos,
    bool              isSticky
) {
    auto* result = origin(pos, isSticky);
    // Piston NBT carries no interlock data, so a client-side piston rebuilt from a block-actor packet would stay
    // InitialNotVisible until the engine's timeout and drop the arm for about three ticks.
    if (smoothPistonRenderEnabled()) mTerrainInterlockData->mRenderVisibilityState = VisibilityState::Visible;
    return result;
}

// The round counter has to advance somewhere that runs once per game tick and independently of the render framerate.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonStepHook,
    ll::memory::HookPriority::Lowest,
    PistonBlockActor,
    &PistonBlockActor::$tick,
    void,
    ::BlockSource& region
) {
    bool const          client      = region.getLevel().isClientSide();
    float const         beforeP     = mProgress;
    float const         beforeLP    = mLastProgress;
    ::PistonState const beforeState = mState;
    auto const          nowTick     = region.getLevel().getCurrentTick().tickID;

    origin(region);

    if (!smoothPistonRenderEnabled()) return;

    // Unconditional, because a hidden piston is never handed to the renderer: a fix that lives in the render hook
    // cannot run for the very frames the arm is missing. Tick is the only path that still executes while hidden.
    auto& interlock = mTerrainInterlockData.get();
    if (interlock.mRenderVisibilityState != VisibilityState::Visible || interlock.mHasBeenDelayedDeleted) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        interlock.mHasBeenDelayedDeleted = false;
    }

    if (!client) return;
    if (beforeP != mProgress || beforeLP != mLastProgress || beforeState != mState) markPistonStepped(this);
    gHeadOwnerRound.store(static_cast<uint64_t>(nowTick), std::memory_order_relaxed);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonArmHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActorRenderer,
    &PistonBlockActorRenderer::$render,
    void,
    ::BaseActorRenderContext& renderContext,
    ::BlockActorRenderData&   blockEntityRenderData
) {
    auto& entity    = blockEntityRenderData.entity;
    auto& interlock = entity.mTerrainInterlockData.get();

    bool const enabled = smoothPistonRenderEnabled();
    bool const hidden  = interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible;

    // Same-cell pistons cannot be found by walking the chunk collections - each instance only ever sees itself there -
    // so one head per cell is elected here instead, fresh every round.
    bool const isArm       = entity.mType == ::BlockActorType::PistonArm;
    bool const selfStepped = isArm && hasPistonStepped(&entity);
    // A piston in a moving_block cell is cargo being pushed by another piston rather than the one driving the
    // animation, so it must lose to an actively extending head instead of competing with it.
    bool const isCargo =
        isArm
        && blockEntityRenderData.renderSource.getBlock(entity.mPosition).getTypeName() == "minecraft:moving_block";
    // Driving heads outrank cargo, and within either role a piston that advanced mProgress outranks an untouched
    // leftover. Equal rank falls back to first-come so exactly one head survives.
    int const selfRank       = (isCargo ? 0 : 2) + (selfStepped ? 1 : 0);
    bool      skipZombieHead = false;
    if (enabled && isArm) {
        auto const            round = gHeadOwnerRound.load(std::memory_order_relaxed);
        std::lock_guard const guard(gHeadOwnerMutex);
        auto&                 owner = gHeadOwners[entity.mPosition.get()];
        if (owner.round != round || owner.actor == nullptr || owner.actor == &entity || selfRank > owner.rank) {
            owner = {round, &entity, selfRank};
        } else {
            skipZombieHead = true;
        }
    }

    if (enabled && hidden) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        // A rebuilt piston also inherits the delete flag, which would re-hide it on the next frame.
        interlock.mHasBeenDelayedDeleted = false;
    }

    if (skipZombieHead) return;

    origin(renderContext, blockEntityRenderData);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackMovingBlockGhostHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActorRenderer,
    &MovingBlockActorRenderer::$render,
    void,
    ::BaseActorRenderContext& renderContext,
    ::BlockActorRenderData&   blockEntityRenderData
) {
    auto&      entity    = blockEntityRenderData.entity;
    bool const isMoving  = entity.mType == ::BlockActorType::MovingBlock;
    auto*      movingPtr = isMoving ? static_cast<::MovingBlockActor*>(&entity) : nullptr;

    if (!smoothPistonRenderEnabled() || !isMoving) {
        origin(renderContext, blockEntityRenderData);
        return;
    }

    auto&      interlock = entity.mTerrainInterlockData.get();
    auto&      source    = blockEntityRenderData.renderSource;
    auto const nowTick   = source.getLevel().getCurrentTick().tickID;

    // A MovingBlock rebuilt mid-animation starts hidden just like the piston does. Only lift that initial state: the
    // retirement logic below owns DelayedDestructionNotVisible and must not be overridden.
    if (interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
    }

    auto const& cellBlock = source.getBlock(entity.mPosition);

    // An air-wrapped MovingBlock has no replacement block of its own coming, so hiding it while its cell is still
    // empty would open a hole. Once a real block occupies the cell there is nothing left for it to cover.
    bool const coversNothing = movingPtr->getWrappedBlock().isAir() && cellBlock.isAir();

    // getDrawPos collapses as soon as mProgress reaches 1, but the renderer keeps lerping mLastProgress towards it for
    // another tick. Both fields at 1 is the only state where no interpolation is left.
    auto* const owner   = movingPtr->getOwningPiston(source);
    bool const  arrived = owner != nullptr && owner->mProgress >= 1.0f && owner->mLastProgress >= 1.0f;

    // Detached means the block entity no longer belongs to this cell: the block has been handed to the next
    // MovingBlock one cell along, so this cell is meant to be empty and keeping it drawn only redraws the block at the
    // position it already left.
    bool const detached = source.getBlockEntity(entity.mPosition) != &entity;

    // A piston body never hands its block to a neighbour, so for it detachment only means the cell re-registered a
    // fresh instance. Retiring it left the cell with neither entity nor replacement block for several frames, which is
    // the piston vanishing mid-extension.
    bool const isPistonBody = movingPtr->getWrappedBlock().getTypeName().find("piston") != std::string::npos;

    if (coversNothing || isPistonBody) {
        // Leave it to vanilla: nothing to hold on screen, nothing to hide.
    } else if (movingPtr->mPreserved && arrived && detached) {
        // Retire on the first frame all three hold. Also requiring a real block in the cell was too strict: the
        // replacement can arrive several frames late and the block stayed visible in its old cell until then.
        interlock.mRenderVisibilityState = VisibilityState::DelayedDestructionNotVisible;
        interlock.mHasBeenDelayedDeleted = true;
    } else if (detached) {
        gTailAnchors[movingPtr] = nowTick;
    }

    origin(renderContext, blockEntityRenderData);
}

// A recycled address must not inherit the previous MovingBlock's tail state.
LL_TYPE_INSTANCE_HOOK(
    PlaybackMovingBlockConstructionHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::$ctor,
    void*,
    ::BlockPos const& pos
) {
    auto* result = origin(pos);
    gTailAnchors.erase(static_cast<::MovingBlockActor const*>(this));
    return result;
}

} // namespace

bool hookPistonRender(bool enable) {
    struct HookState {
        bool arm{};
        bool armRender{};
        bool step{};
        bool ghost{};
        bool movingCtor{};
    };
    static HookState state;

    auto installAll = [&] {
        if (!state.arm) state.arm = PlaybackPistonArmVisibilityHook::hook() == 0;
        if (!state.arm) return false;
        if (!state.armRender) state.armRender = PlaybackPistonArmHook::hook() == 0;
        if (!state.armRender) return false;
        if (!state.step) state.step = PlaybackPistonStepHook::hook() == 0;
        if (!state.step) return false;
        if (!state.ghost) state.ghost = PlaybackMovingBlockGhostHook::hook() == 0;
        if (!state.ghost) return false;
        if (!state.movingCtor) state.movingCtor = PlaybackMovingBlockConstructionHook::hook() == 0;
        return state.movingCtor;
    };

    if (enable) {
        if (gInstalled.load(std::memory_order_acquire)) return true;
        if (!installAll()) return false;
        gInstalled.store(true, std::memory_order_release);
        return true;
    }

    if (!gInstalled.load(std::memory_order_acquire)) return true;

    if (state.movingCtor) {
        PlaybackMovingBlockConstructionHook::unhook();
        state.movingCtor = false;
    }
    if (state.ghost) {
        PlaybackMovingBlockGhostHook::unhook();
        state.ghost = false;
    }
    if (state.step) {
        PlaybackPistonStepHook::unhook();
        state.step = false;
    }
    if (state.armRender) {
        PlaybackPistonArmHook::unhook();
        state.armRender = false;
    }
    if (state.arm) {
        PlaybackPistonArmVisibilityHook::unhook();
        state.arm = false;
    }

    gTailAnchors.clear();
    {
        std::lock_guard const guard(gSteppedPistonMutex);
        gSteppedPistons.clear();
    }
    {
        std::lock_guard const guard(gHeadOwnerMutex);
        gHeadOwners.clear();
    }

    gInstalled.store(false, std::memory_order_release);
    return true;
}

bool isPistonRenderInstalled() noexcept { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::visuals
