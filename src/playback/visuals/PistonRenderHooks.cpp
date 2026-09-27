#include "PistonRenderHooks.h"

#include "playback/Playback.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/blockactor/BlockActorRenderData.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/blockactor/PistonBlockActorRenderer.h"
#include "mc/client/renderer/chunks/RenderChunkCoordinator.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/UpdateSubChunkBlocksChangedInfo.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/network/packet/UpdateSubChunkNetworkBlockInfo.h"
#include "mc/world/actor/ActorTerrainInterlockData.h"
#include "mc/world/level/BlockPalette.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/MovingBlockActor.h"
#include "mc/world/level/block/actor/PistonBlockActor.h"
#include "mc/world/level/block/actor/PistonState.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace playback::visuals {

namespace {

using VisibilityState = ::ActorTerrainInterlockData::VisibilityState;
using replay::ReplaySession;

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

// ---- Render-only 3gt animation on the replay clock; native progress, state and world blocks stay untouched ----

constexpr int VisualTicks = 3;

// Non-zero while a piston or moving-block renderer is on this thread's stack; logic callers keep native values.
thread_local int tRenderDepth = 0;

struct RenderScope {
    RenderScope() { ++tRenderDepth; }
    ~RenderScope() { --tRenderDepth; }
    RenderScope(RenderScope const&)            = delete;
    RenderScope& operator=(RenderScope const&) = delete;
};

struct VisualSegment {
    int   startTick{};
    float from{};
    float to{};
};

struct PistonVisual {
    uint64_t      action{};
    VisualSegment current;
    // A truncated predecessor keeps drawing the arm until the new action's start tick.
    std::optional<VisualSegment> previous;
};

struct ActionVisual {
    VisualSegment segment;
    ::BlockPos    facing;
};

// One MovingBlock target cell owned by an action.
struct CellClaim {
    uint64_t    action{};
    std::string wrapped;
    bool        landed{};   // the real block arrived and its chunk mesh is suppressed
    bool        released{}; // visual ended, the real block is being re-meshed
    bool        rebuilt{};
    uint64_t    rebuiltLt{};
};

std::mutex gAnimMutex;
uint64_t   gNextAction = 1;
int        gLastSweepTick{-1};
// Keyed by position: replay rebuilds piston instances mid-action.
std::unordered_map<::BlockPos, PistonVisual, BlockPosHash> gPistonVisuals;
std::unordered_map<uint64_t, ActionVisual>                 gActionVisuals;
std::unordered_map<::MovingBlockActor const*, uint64_t>    gMovingAction;
std::unordered_map<::BlockPos, CellClaim, BlockPosHash>    gCellClaims;
std::atomic_bool                                           gMeshWatch{false};
std::atomic<uint64_t>                                      gFrameLt{0};

bool replayActive() { return ReplaySession::getInstance().isIsolatingReplayWorld(); }

bool animationActive() { return smoothPistonRenderEnabled() && replayActive(); }

// Native interpolation at applied tick rt draws time rt - 1 + a.
double visualTime(float alpha) { return ReplaySession::getInstance().getAppliedReplayTick() - 1.0 + alpha; }

float segmentValue(VisualSegment const& segment, double time) {
    auto const f = std::clamp((time - segment.startTick) / VisualTicks, 0.0, 1.0);
    return segment.from + (segment.to - segment.from) * static_cast<float>(f);
}

float armValue(PistonVisual const& visual, double time) {
    if (visual.previous && time < visual.current.startTick) return segmentValue(*visual.previous, time);
    return segmentValue(visual.current, time);
}

void updateMeshWatchLocked() {
    gMeshWatch.store(
        std::any_of(
            gCellClaims.begin(),
            gCellClaims.end(),
            [](auto const& entry) { return entry.second.landed && !entry.second.rebuilt; }
        ),
        std::memory_order_relaxed
    );
}

// Returns cells whose mesh is still missing the landed block.
std::vector<::BlockPos> clearAnimationStateLocked() {
    std::vector<::BlockPos> cells;
    for (auto const& [pos, claim] : gCellClaims)
        if (claim.landed && !claim.rebuilt) cells.push_back(pos);
    gPistonVisuals.clear();
    gActionVisuals.clear();
    gMovingAction.clear();
    gCellClaims.clear();
    gLastSweepTick = -1;
    updateMeshWatchLocked();
    return cells;
}

void clearAnimationState() {
    std::lock_guard const guard(gAnimMutex);
    clearAnimationStateLocked();
}

// Caller holds gAnimMutex. Returns landed cells whose real block must be re-meshed now.
std::vector<::BlockPos> endActionLocked(uint64_t action) {
    gActionVisuals.erase(action);
    std::vector<::BlockPos> cells;
    for (auto it = gCellClaims.begin(); it != gCellClaims.end();) {
        auto& claim = it->second;
        if (claim.action != action || claim.released) {
            ++it;
        } else if (!claim.landed) {
            it = gCellClaims.erase(it);
        } else {
            claim.released = true;
            cells.push_back(it->first);
            ++it;
        }
    }
    return cells;
}

void rebuildCells(::BlockSource& region, std::vector<::BlockPos> const& cells) {
    for (auto const& pos : cells) region.fireAreaChanged(pos, pos);
}

// Ends visuals past their duration, or ahead of the clock after a backward seek.
void sweepActions(::BlockSource& region) {
    std::vector<::BlockPos> cells;
    {
        std::lock_guard const guard(gAnimMutex);
        if (!animationActive()) {
            if (gLastSweepTick != -1 || !gCellClaims.empty()) cells = clearAnimationStateLocked();
        }
    }
    if (!cells.empty()) {
        rebuildCells(region, cells);
        return;
    }
    {
        std::lock_guard const guard(gAnimMutex);
        if (!animationActive()) return;
        auto const tick = ReplaySession::getInstance().getAppliedReplayTick();
        if (tick == gLastSweepTick) return;
        gLastSweepTick     = tick;
        auto const expired = [tick](VisualSegment const& segment) {
            return tick > segment.startTick + VisualTicks || tick < segment.startTick;
        };
        std::erase_if(gPistonVisuals, [&](auto const& entry) { return expired(entry.second.current); });
        std::vector<uint64_t> ended;
        for (auto const& [action, visual] : gActionVisuals)
            if (expired(visual.segment)) ended.push_back(action);
        for (auto const action : ended) {
            auto released = endActionLocked(action);
            cells.insert(cells.end(), released.begin(), released.end());
        }
        updateMeshWatchLocked();
    }
    rebuildCells(region, cells);
}

// Worker threads build chunk meshes; the MovingBlock renderer tessellates on the render thread and must pass.
bool meshSuppressed(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gMeshWatch.load(std::memory_order_relaxed)) return false;
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCellClaims.find(pos);
    if (it == gCellClaims.end() || !it->second.landed) return false;
    auto& claim = it->second;
    if (!claim.released) return true;
    if (!claim.rebuilt) {
        claim.rebuilt   = true;
        claim.rebuiltLt = gFrameLt.load(std::memory_order_relaxed);
        updateMeshWatchLocked();
    }
    return false;
}

std::string blockNameOf(uint runtimeId) {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return {};
    return player->getLevel().getBlockPalette().getBlock(runtimeId).getTypeName();
}

void startAction(::BlockSource& region, ::BlockPos const& pistonPos, ::BlockPos const& facing, bool extending) {
    std::vector<::BlockPos> cells;
    {
        std::lock_guard const guard(gAnimMutex);
        auto const            tick = ReplaySession::getInstance().getAppliedReplayTick();
        float const           to   = extending ? 1.0f : 0.0f;
        PistonVisual          visual{
            gNextAction++,
            {tick, 1.0f - to, to},
            std::nullopt
        };
        if (auto it = gPistonVisuals.find(pistonPos); it != gPistonVisuals.end()) {
            auto const& old     = it->second;
            bool const  running = tick >= old.current.startTick && tick <= old.current.startTick + VisualTicks;
            // A piston cannot start the same direction twice, so this is a resent packet, not a new action.
            if (running && old.current.to == to) return;
            // Chain policy: truncate the old action and continue from where the arm is drawn.
            if (running) {
                visual.current.from = armValue(old, tick);
                visual.previous     = old.current;
            }
            cells = endActionLocked(old.action);
        }
        gActionVisuals[visual.action] = {visual.current, facing};
        gPistonVisuals[pistonPos]     = visual;
        updateMeshWatchLocked();
    }
    rebuildCells(region, cells);
}

std::optional<float> visualArmProgress(::BlockPos const& pistonPos, float alpha) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gPistonVisuals.find(pistonPos);
    if (it == gPistonVisuals.end()) return std::nullopt;
    return armValue(it->second, visualTime(alpha));
}

// Relative to the MovingBlock's own (target) cell, same convention as native getDrawPos.
std::optional<::Vec3> visualDrawOffset(::MovingBlockActor const& moving, float alpha) {
    std::lock_guard const guard(gAnimMutex);
    auto const            owned = gMovingAction.find(&moving);
    if (owned == gMovingAction.end()) return std::nullopt;
    auto const it = gActionVisuals.find(owned->second);
    if (it == gActionVisuals.end()) {
        // Visual ended but still held for the re-mesh: stay exactly on the cell.
        auto const claim = gCellClaims.find(moving.mPosition.get());
        if (claim != gCellClaims.end() && claim->second.action == owned->second) return ::Vec3{0.0f, 0.0f, 0.0f};
        return std::nullopt;
    }
    auto const& segment = it->second.segment;
    auto const& facing  = it->second.facing;
    auto const  rel     = segmentValue(segment, visualTime(alpha)) - segment.to;
    return ::Vec3{
        static_cast<float>(facing.x) * rel,
        static_cast<float>(facing.y) * rel,
        static_cast<float>(facing.z) * rel
    };
}

void registerMoving(::MovingBlockActor const& moving) {
    std::lock_guard const guard(gAnimMutex);
    if (gMovingAction.contains(&moving)) return;
    auto const it = gPistonVisuals.find(moving.mPistonBlockPos.get());
    if (it == gPistonVisuals.end() || !gActionVisuals.contains(it->second.action)) return;
    auto const action      = it->second.action;
    gMovingAction[&moving] = action;
    auto const cell        = moving.mPosition.get();
    auto const claim       = gCellClaims.find(cell);
    // The block now moves out of its source cell, so an older claim there would keep drawing a stale copy.
    if (auto const visual = gActionVisuals.find(action); visual != gActionVisuals.end()) {
        auto const&      facing = visual->second.facing;
        int const        dir    = visual->second.segment.to > visual->second.segment.from ? 1 : -1;
        ::BlockPos const source{cell.x - facing.x * dir, cell.y - facing.y * dir, cell.z - facing.z * dir};
        auto const       old = source == moving.mPistonBlockPos.get() ? gCellClaims.end() : gCellClaims.find(source);
        if (old != gCellClaims.end() && old->second.action != action) {
            gCellClaims.erase(old);
            updateMeshWatchLocked();
        }
    }
    // An older claim still hiding its landed block keeps the cell until it is re-meshed.
    if (claim != gCellClaims.end() && claim->second.landed && !claim->second.released) return;
    if (claim != gCellClaims.end() && claim->second.action == action) return;
    gCellClaims[cell] = {action, moving.getWrappedBlock().getTypeName()};
    updateMeshWatchLocked();
}

// Held while its action plays, then until the real block has been re-meshed and shown for a couple of frames.
bool movingHeld(::MovingBlockActor const& moving) {
    std::lock_guard const guard(gAnimMutex);
    auto const            owned = gMovingAction.find(&moving);
    if (owned == gMovingAction.end()) return false;
    if (gActionVisuals.contains(owned->second)) return true;
    auto const claim = gCellClaims.find(moving.mPosition.get());
    if (claim == gCellClaims.end() || claim->second.action != owned->second) return false;
    if (!claim->second.rebuilt) return true;
    // Tessellation finishes on a worker; the new mesh is uploaded on a later frame.
    if (gFrameLt.load(std::memory_order_relaxed) <= claim->second.rebuiltLt + 2) return true;
    gCellClaims.erase(claim);
    updateMeshWatchLocked();
    return false;
}

bool hasClaims() {
    std::lock_guard const guard(gAnimMutex);
    return !gCellClaims.empty();
}

// Runs before the packet applies, so the mesh rebuild it triggers already skips the landed block.
void holdLandedCell(::BlockPos const& cell, std::string const& block) {
    if (block.empty() || block == "minecraft:air" || block == "minecraft:moving_block") return;
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCellClaims.find(cell);
    if (it == gCellClaims.end() || it->second.released || !gActionVisuals.contains(it->second.action)) return;
    it->second.landed = true;
    updateMeshWatchLocked();
}

void forgetMoving(::MovingBlockActor const* moving) {
    std::lock_guard const guard(gAnimMutex);
    gMovingAction.erase(moving);
}

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

    // Also runs when disabled, so meshes hidden by an unfinished visual get rebuilt.
    if (client) sweepActions(region);
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

    if (enabled) gFrameLt.store(blockEntityRenderData.renderSource.getLevel().getCurrentTick().tickID);
    RenderScope const scope;
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

    RenderScope const scope;

    if (!smoothPistonRenderEnabled() || !isMoving) {
        origin(renderContext, blockEntityRenderData);
        return;
    }

    auto&      interlock = entity.mTerrainInterlockData.get();
    auto&      source    = blockEntityRenderData.renderSource;
    auto const nowTick   = source.getLevel().getCurrentTick().tickID;
    gFrameLt.store(nowTick);

    // A MovingBlock rebuilt mid-animation starts hidden just like the piston does. Only lift that initial state: the
    // retirement logic below owns DelayedDestructionNotVisible and must not be overridden.
    if (interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
    }

    if (replayActive()) {
        registerMoving(*movingPtr);
        // The visual owns this block until the real block is re-meshed; native retirement would cut it short.
        if (movingHeld(*movingPtr)) {
            interlock.mRenderVisibilityState = VisibilityState::Visible;
            interlock.mHasBeenDelayedDeleted = false;
            origin(renderContext, blockEntityRenderData);
            return;
        }
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
    forgetMoving(this);
    return result;
}

// Starts the visual when replay flips a piston into motion; the native state change itself is untouched.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonActionHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$_onUpdatePacket,
    void,
    ::CompoundTag const& data,
    ::BlockSource&       region
) {
    ::PistonState const before = mState;
    origin(data, region);
    if (!animationActive() || mState == before) return;
    if (mState != ::PistonState::Expanding && mState != ::PistonState::Retracting) return;
    startAction(region, mPosition.get(), getFacingDir(region), mState == ::PistonState::Expanding);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonProgressHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::getProgress,
    float,
    float a
) {
    if (tRenderDepth > 0 && animationActive()) {
        if (auto const visual = visualArmProgress(mPosition.get(), a)) return *visual;
    }
    return origin(a);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackMovingDrawPosHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::getDrawPos,
    ::Vec3,
    ::IConstBlockSource const& region,
    float                      a
) {
    if (tRenderDepth > 0 && animationActive()) {
        if (auto const offset = visualDrawOffset(*this, a)) return *offset;
    }
    return origin(region, a);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackLandedBlockHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&          source,
    ::UpdateSubChunkBlocksPacket const& packet
) {
    if (animationActive() && hasClaims()) {
        for (auto const& info : *packet.mBlocksChanged->mStandards)
            holdLandedCell(info.mPos, blockNameOf(info.mRuntimeId));
    }
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackMeshInWorldHook,
    ll::memory::HookPriority::Normal,
    BlockTessellator,
    static_cast<bool (::BlockTessellator::*)(::Tessellator&, ::Block const&, ::BlockPos const&, bool)>(
        &::BlockTessellator::tessellateInWorld
    ),
    bool,
    ::Tessellator&    tessellator,
    ::Block const&    block,
    ::BlockPos const& pos,
    bool              useCalcWithCache
) {
    if (meshSuppressed(pos)) return false;
    return origin(tessellator, block, pos, useCalcWithCache);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackMeshBlockInWorldHook,
    ll::memory::HookPriority::Normal,
    BlockTessellator,
    &BlockTessellator::tessellateBlockInWorld,
    bool,
    ::Tessellator&                 tessellator,
    ::Block const&                 block,
    ::BlockPos const&              pos,
    std::bitset<6>                 faces,
    ::AirAndSimpleBlockBits const* airAndSimpleBlocks
) {
    if (meshSuppressed(pos)) return false;
    return origin(tessellator, block, pos, faces, airAndSimpleBlocks);
}

} // namespace

bool hookPistonRender(bool enable) {
    struct HookState {
        bool arm{};
        bool armRender{};
        bool step{};
        bool ghost{};
        bool movingCtor{};
        bool action{};
        bool progress{};
        bool drawPos{};
        bool landed{};
        bool meshInWorld{};
        bool meshBlockInWorld{};
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
        if (!state.movingCtor) return false;
        if (!state.meshInWorld) state.meshInWorld = PlaybackMeshInWorldHook::hook() == 0;
        if (!state.meshInWorld) return false;
        if (!state.meshBlockInWorld) state.meshBlockInWorld = PlaybackMeshBlockInWorldHook::hook() == 0;
        if (!state.meshBlockInWorld) return false;
        if (!state.landed) state.landed = PlaybackLandedBlockHook::hook() == 0;
        if (!state.landed) return false;
        if (!state.progress) state.progress = PlaybackPistonProgressHook::hook() == 0;
        if (!state.progress) return false;
        if (!state.drawPos) state.drawPos = PlaybackMovingDrawPosHook::hook() == 0;
        if (!state.drawPos) return false;
        // Last, so no visual starts before everything that finishes it is in place.
        if (!state.action) state.action = PlaybackPistonActionHook::hook() == 0;
        return state.action;
    };

    if (enable) {
        if (gInstalled.load(std::memory_order_acquire)) return true;
        if (!installAll()) return false;
        gInstalled.store(true, std::memory_order_release);
        return true;
    }

    if (!gInstalled.load(std::memory_order_acquire)) return true;

    if (state.action) {
        PlaybackPistonActionHook::unhook();
        state.action = false;
    }
    if (state.drawPos) {
        PlaybackMovingDrawPosHook::unhook();
        state.drawPos = false;
    }
    if (state.progress) {
        PlaybackPistonProgressHook::unhook();
        state.progress = false;
    }
    if (state.landed) {
        PlaybackLandedBlockHook::unhook();
        state.landed = false;
    }
    if (state.meshBlockInWorld) {
        PlaybackMeshBlockInWorldHook::unhook();
        state.meshBlockInWorld = false;
    }
    if (state.meshInWorld) {
        PlaybackMeshInWorldHook::unhook();
        state.meshInWorld = false;
    }
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
    clearAnimationState();

    gInstalled.store(false, std::memory_order_release);
    return true;
}

bool isPistonRenderInstalled() noexcept { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::visuals
