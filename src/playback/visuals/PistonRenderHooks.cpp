#include "PistonRenderHooks.h"

#include "playback/Playback.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/block/BlockOccluder.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/blockactor/BlockActorRenderData.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/blockactor/PistonBlockActorRenderer.h"
#include "mc/client/renderer/chunks/RenderChunkCoordinator.h"
#include "mc/client/renderer/chunks/RenderChunkGeometry.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/UpdateSubChunkBlocksChangedInfo.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/network/packet/UpdateSubChunkNetworkBlockInfo.h"
#include "mc/platform/threading/Mutex.h"
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
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/LevelChunkBlockActorStorage.h"
#include "mc/world/phys/AABB.h"

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

// ---- Render-only 4gt animation on the replay clock; native progress, state and world blocks stay untouched ----

constexpr int VisualTicks = 4;

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

struct ActionVisual {
    VisualSegment segment;
    ::BlockPos    facing;
};

struct PistonVisual {
    uint64_t      action{};
    VisualSegment current;
    // A truncated predecessor keeps drawing the arm until the new action's start tick.
    std::optional<VisualSegment> previous;
    // Tail of the push that carried this piston's body here, still playing when its own action starts.
    std::optional<ActionVisual> bodyCarry;
};

// Tail of the action that carried a block into the cell a newer action picks it up from.
struct CarryVisual {
    uint64_t     action{};
    ActionVisual visual;
};

// One MovingBlock target cell owned by an action.
struct CellClaim {
    uint64_t    action{};
    std::string wrapped;
    bool        landed{};    // the real block arrived and its chunk mesh is suppressed
    bool        released{};  // visual ended, the real block is being re-meshed
    bool        rebuilt{};   // a build containing the real block has committed
    uint64_t    liveFrame{}; // frame of that commit; the mesh is on screen from the next frame
};

std::mutex gAnimMutex;
uint64_t   gNextAction = 1;
int        gLastSweepTick{-1};
bool       gSweepReplay{}; // clock the current visuals are timed on
// Keyed by position: replay rebuilds piston instances mid-action.
std::unordered_map<::BlockPos, PistonVisual, BlockPosHash> gPistonVisuals;
std::unordered_map<uint64_t, ActionVisual>                 gActionVisuals;
struct MovingEntry {
    uint64_t     action{};
    ::BlockPos   cell;
    ActionVisual visual; // copied, so a successor can still replay its tail after the action ends
    // A newer action already draws this block, so this copy is never drawn again.
    bool handedOff{};
    // Its cell is re-meshed with the real block, so drawing it again would double it.
    bool                       released{};
    std::optional<CarryVisual> carry;
    // The chunk may destroy a detached instance before its cell is re-meshed.
    std::shared_ptr<::BlockActor> keep;
};
// Released outside gAnimMutex because the dtor hook locks it.
std::vector<std::shared_ptr<::BlockActor>> gDropKeep;

std::unordered_map<::MovingBlockActor const*, MovingEntry> gMovingAction;
std::unordered_map<::BlockPos, CellClaim, BlockPosHash>    gCellClaims;
std::atomic_bool                                           gMeshWatch{false};
std::atomic_bool                                           gFaceWatch{false}; // any unreleased claim
std::atomic<uint64_t>                                      gFrameLt{0};
// Counts frames that drew block actors; export also renders passes without them.
std::atomic<uint64_t> gFrame{1};
std::atomic_bool      gFrameDrewActors{false};
// Chunk geometry being tessellated on this thread.
thread_local ::RenderChunkGeometry const* tMeshGeometry = nullptr;
// Released cells tessellated as real blocks, per build that has not been uploaded yet.
std::unordered_map<::RenderChunkGeometry const*, std::vector<::BlockPos>> gReleasedMesh;

bool replayActive() { return ReplaySession::getInstance().isIsolatingReplayWorld(); }

// Replay ticks on its own clock; in a live world the client level tick runs 1:1 with redstone.
std::optional<int> visualClockTick() {
    if (replayActive()) return ReplaySession::getInstance().getAppliedReplayTick();
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return std::nullopt;
    return static_cast<int>(player->getLevel().getCurrentTick().tickID);
}

bool animationActive() { return smoothPistonRenderEnabled() && visualClockTick().has_value(); }

// Native interpolation at tick t draws time t - 1 + a.
double visualTime(float alpha) { return visualClockTick().value_or(0) - 1.0 + alpha; }

float segmentValue(VisualSegment const& segment, double time) {
    auto const f = std::clamp((time - segment.startTick) / VisualTicks, 0.0, 1.0);
    return segment.from + (segment.to - segment.from) * static_cast<float>(f);
}

// An interrupted predecessor keeps playing its remaining tail on top of the new action.
float armValue(PistonVisual const& visual, double time) {
    auto value = segmentValue(visual.current, time);
    if (visual.previous) value += segmentValue(*visual.previous, time) - visual.previous->to;
    return std::clamp(value, 0.0f, 1.0f);
}

// Relative to the target cell, same convention as native getDrawPos.
::Vec3 actionOffset(ActionVisual const& visual, double time) {
    auto const rel = segmentValue(visual.segment, time) - visual.segment.to;
    return ::Vec3{
        static_cast<float>(visual.facing.x) * rel,
        static_cast<float>(visual.facing.y) * rel,
        static_cast<float>(visual.facing.z) * rel
    };
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
    gFaceWatch.store(
        std::any_of(gCellClaims.begin(), gCellClaims.end(), [](auto const& entry) { return !entry.second.released; }),
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
    for (auto& [moving, entry] : gMovingAction)
        if (entry.keep) gDropKeep.push_back(std::move(entry.keep));
    gMovingAction.clear();
    gCellClaims.clear();
    gReleasedMesh.clear();
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

// Set while our own re-mesh requests pass through the level listeners.
thread_local bool tImmediateRebuild = false;

void rebuildCells(::BlockSource& region, std::vector<::BlockPos> const& cells) {
    tImmediateRebuild = true;
    // Neighbours kept faces against the hidden block and may sit in another subchunk.
    for (auto const& pos : cells) region.fireAreaChanged(pos - ::BlockPos{1, 1, 1}, pos + ::BlockPos{1, 1, 1});
    tImmediateRebuild = false;
}

// Caller holds gAnimMutex. Visual ticks mean nothing after a switch between replay and live clocks or a backward seek.
bool clockInvalidatedLocked() {
    bool const replay = replayActive();
    bool const stale =
        gLastSweepTick != -1 && (replay != gSweepReplay || visualClockTick().value_or(0) < gLastSweepTick);
    gSweepReplay = replay;
    return stale;
}

// Ends visuals past their duration, or ahead of the clock after a backward seek.
void sweepActions(::BlockSource& region) {
    std::vector<::BlockPos> cells;
    {
        std::lock_guard const guard(gAnimMutex);
        if (!animationActive() || clockInvalidatedLocked()) {
            if (gLastSweepTick != -1 || !gCellClaims.empty() || !gMovingAction.empty())
                cells = clearAnimationStateLocked();
        }
    }
    if (!cells.empty()) {
        rebuildCells(region, cells);
        return;
    }
    {
        std::lock_guard const guard(gAnimMutex);
        if (!animationActive()) return;
        auto const tick = visualClockTick().value_or(0);
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
    // Counts only once this build is uploaded; a superseded build never reaches endRebuild.
    if (!claim.rebuilt && tMeshGeometry) gReleasedMesh[tMeshGeometry].push_back(pos);
    return false;
}

// A newer build of the same geometry replaces the pending one.
void meshBuildStarted(::RenderChunkGeometry const* geometry) {
    std::lock_guard const guard(gAnimMutex);
    gReleasedMesh.erase(geometry);
}

// Runs on the frame thread once the rebuilt geometry is live.
void meshBuildCommitted(::RenderChunkGeometry const* geometry) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gReleasedMesh.find(geometry);
    if (it == gReleasedMesh.end()) return;
    auto const frame = gFrame.load(std::memory_order_relaxed);
    for (auto const& pos : it->second) {
        auto const claim = gCellClaims.find(pos);
        if (claim == gCellClaims.end() || !claim->second.released || claim->second.rebuilt) continue;
        claim->second.rebuilt   = true;
        claim->second.liveFrame = frame;
    }
    gReleasedMesh.erase(it);
    updateMeshWatchLocked();
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
        // A packet can arrive before the first sweep on the new clock.
        if (clockInvalidatedLocked()) cells = clearAnimationStateLocked();
        auto const   tick = visualClockTick().value_or(0);
        float const  to   = extending ? 1.0f : 0.0f;
        PistonVisual visual{
            gNextAction++,
            {tick, 1.0f - to, to},
            std::nullopt,
            std::nullopt
        };
        if (auto it = gPistonVisuals.find(pistonPos); it != gPistonVisuals.end()) {
            auto const& old     = it->second;
            bool const  running = tick >= old.current.startTick && tick <= old.current.startTick + VisualTicks;
            // A piston cannot start the same direction twice, so this is a resent packet, not a new action.
            if (running && old.current.to == to) return;
            // Chain policy: the old action's remaining tail plays on top of the new one.
            if (running) visual.previous = old.current;
            cells = endActionLocked(old.action);
        }
        // This piston's body may still be arriving from a push; its arm must follow that motion until it ends.
        for (auto const& [moving, entry] : gMovingAction) {
            if (entry.cell != pistonPos || entry.handedOff) continue;
            // A finished tail contributes zero offset, so only the newest carrier matters.
            if (!visual.bodyCarry || entry.visual.segment.startTick > visual.bodyCarry->segment.startTick)
                visual.bodyCarry = entry.visual;
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
    auto const& entry = owned->second;
    if (!gActionVisuals.contains(entry.action)) {
        // Visual ended but still held for the re-mesh: stay exactly on the cell.
        auto const claim = gCellClaims.find(entry.cell);
        if (claim != gCellClaims.end() && claim->second.action == entry.action) return ::Vec3{0.0f, 0.0f, 0.0f};
        return std::nullopt;
    }
    auto const time   = visualTime(alpha);
    auto       offset = actionOffset(entry.visual, time);
    if (entry.carry) {
        auto const tail  = actionOffset(entry.carry->visual, time);
        offset.x        += tail.x;
        offset.y        += tail.y;
        offset.z        += tail.z;
    }
    return offset;
}

// Extra offset for the arm of a piston whose body is still finishing the push that carried it here.
std::optional<::Vec3> visualBodyOffset(::BlockPos const& pistonPos, float alpha) {
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gPistonVisuals.find(pistonPos);
    if (it == gPistonVisuals.end() || !it->second.bodyCarry) return std::nullopt;
    return actionOffset(*it->second.bodyCarry, visualTime(alpha));
}

// Caller holds gAnimMutex; the dtor hook takes it under the chunk lock, so the chunk lock is only tried.
std::shared_ptr<::BlockActor> findOwner(::BlockSource& region, ::MovingBlockActor const& moving) {
    auto* const chunk = region.getChunkAt(moving.mPosition.get());
    if (chunk == nullptr) return {};
    std::unique_lock<std::mutex> const lock(chunk->mBlockEntityAccessLock.get(), std::try_to_lock);
    if (!lock.owns_lock()) return {};
    auto const* const actor = static_cast<::BlockActor const*>(&moving);
    for (auto const& [pos, owned] : chunk->mBlockEntities.get().mMap.get())
        if (owned.get() == actor) return owned;
    for (auto const& owned : chunk->mPreservedBlockEntities.get())
        if (owned.get() == actor) return owned;
    return {};
}

void registerMoving(::MovingBlockActor const& moving, ::BlockSource& region) {
    std::lock_guard const guard(gAnimMutex);
    if (gMovingAction.contains(&moving)) return;
    auto const it = gPistonVisuals.find(moving.mPistonBlockPos.get());
    if (it == gPistonVisuals.end()) return;
    auto const visual = gActionVisuals.find(it->second.action);
    if (visual == gActionVisuals.end()) return;
    auto const  action = it->second.action;
    auto const  cell   = moving.mPosition.get();
    MovingEntry entry{action, cell, visual->second};
    // The block now leaves its source cell; an older MovingBlock there still playing its tail hands it over.
    auto const&      facing = visual->second.facing;
    int const        dir    = visual->second.segment.to > visual->second.segment.from ? 1 : -1;
    ::BlockPos const source{cell.x - facing.x * dir, cell.y - facing.y * dir, cell.z - facing.z * dir};
    if (source != moving.mPistonBlockPos.get()) {
        for (auto& [other, old] : gMovingAction) {
            if (old.cell != source || old.action == action || old.handedOff) continue;
            old.handedOff = true;
            if (!entry.carry || old.visual.segment.startTick > entry.carry->visual.segment.startTick)
                entry.carry = CarryVisual{old.action, old.visual};
        }
        // The old claim would keep its stale copy held in a cell the block already left.
        if (auto const old = gCellClaims.find(source); old != gCellClaims.end() && old->second.action != action) {
            gCellClaims.erase(old);
            updateMeshWatchLocked();
        }
    }
    entry.keep             = findOwner(region, moving);
    gMovingAction[&moving] = entry;
    auto const claim       = gCellClaims.find(cell);
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
    if (gActionVisuals.contains(owned->second.action)) return true;
    auto const claim = gCellClaims.find(owned->second.cell);
    if (claim == gCellClaims.end() || claim->second.action != owned->second.action) return false;
    // The real block is on screen only from the frame after its mesh was uploaded.
    if (!claim->second.rebuilt || gFrame.load(std::memory_order_relaxed) <= claim->second.liveFrame) return true;
    // Every MovingBlock sharing this claim is covered by the same rebuilt mesh.
    for (auto& [other, entry] : gMovingAction)
        if (entry.action == claim->second.action && entry.cell == claim->first) entry.released = true;
    gCellClaims.erase(claim);
    updateMeshWatchLocked();
    return false;
}

bool movingRetired(::MovingBlockActor const& moving) {
    std::lock_guard const guard(gAnimMutex);
    auto const            owned = gMovingAction.find(&moving);
    return owned != gMovingAction.end() && (owned->second.handedOff || owned->second.released);
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
    auto const            it = gMovingAction.find(moving);
    if (it == gMovingAction.end()) return;
    if (it->second.keep) gDropKeep.push_back(std::move(it->second.keep));
    gMovingAction.erase(it);
}

// Chunk meshes skip faces against the target cell, but it stays empty on screen while the MovingBlock is still
// gliding in. Returns 0 if shown, 1 landed, 2 still a moving_block.
int neighborHidden(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gFaceWatch.load(std::memory_order_relaxed)) return 0;
    std::lock_guard const guard(gAnimMutex);
    auto const            it = gCellClaims.find(pos);
    if (it == gCellClaims.end() || it->second.released) return 0;
    return it->second.landed ? 1 : 2;
}

// Full cubes cull against simple neighbours through the chunk's bitset, never asking the occluder.
bool touchesLanded(::BlockPos const& pos) {
    if (tRenderDepth > 0 || !gFaceWatch.load(std::memory_order_relaxed)) return false;
    static ::BlockPos const dirs[6]{
        {0,  -1, 0 },
        {0,  1,  0 },
        {0,  0,  -1},
        {0,  0,  1 },
        {-1, 0,  0 },
        {1,  0,  0 }
    };
    for (auto const& dir : dirs)
        if (neighborHidden(pos + dir) == 1) return true;
    return false;
}

// Caller holds gAnimMutex. Same rule as movingHeld, without its unhold side effect.
bool heldLocked(MovingEntry const& entry) {
    if (entry.handedOff || entry.released) return false;
    if (gActionVisuals.contains(entry.action)) return true;
    auto const claim = gCellClaims.find(entry.cell);
    return claim != gCellClaims.end() && claim->second.action == entry.action;
}

// A MovingBlock created in a cell pushed again right after landing can miss the camera's collection for several
// ticks.
void queueFreshMoving(::LevelRendererCamera& camera) {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto&                   region = player->getDimensionBlockSource();
    std::vector<::BlockPos> cells;
    {
        std::lock_guard const   guard(gAnimMutex);
        static ::BlockPos const dirs[7]{
            {0,  0,  0 },
            {0,  -1, 0 },
            {0,  1,  0 },
            {0,  0,  -1},
            {0,  0,  1 },
            {-1, 0,  0 },
            {1,  0,  0 }
        };
        std::unordered_set<::BlockPos, BlockPosHash> seen;
        for (auto const& [pos, claim] : gCellClaims)
            for (auto const& dir : dirs)
                if (seen.insert(pos + dir).second) cells.push_back(pos + dir);
    }
    auto& queue = camera.mBlockActorRenderQueue.get();
    auto& alpha = camera.mBlockActorRenderAlphaQueue.get();
    for (auto const& pos : cells) {
        auto* actor = region.getBlockEntity(pos);
        if (!actor || actor->mType != ::BlockActorType::MovingBlock) continue;
        auto const same = [actor](auto const& item) { return item.get() == actor; };
        if (std::any_of(queue.begin(), queue.end(), same) || std::any_of(alpha.begin(), alpha.end(), same)) continue;
        queue.emplace_back(actor);
    }
}

// Runs right after the camera re-collects its queues, the only point where a kept instance can be let go
// safely.
void requeueHeldMoving(::LevelRendererCamera& camera) {
    std::vector<std::shared_ptr<::BlockActor>> drop;
    {
        std::lock_guard const guard(gAnimMutex);
        auto&                 queue  = camera.mBlockActorRenderQueue.get();
        auto&                 alpha  = camera.mBlockActorRenderAlphaQueue.get();
        auto&                 shadow = camera.mBlockActorShadowQueue.get();
        auto const            queued = [](auto const& list, ::BlockActor const* actor) {
            return std::any_of(list.begin(), list.end(), [actor](auto const& item) { return item.get() == actor; });
        };
        for (auto& [moving, entry] : gMovingAction) {
            auto* actor = static_cast<::BlockActor*>(const_cast<::MovingBlockActor*>(moving));
            if (!heldLocked(entry)) {
                if (entry.keep) drop.push_back(std::move(entry.keep));
                continue;
            }
            if (queued(queue, actor) || queued(alpha, actor)) continue;
            queue.emplace_back(actor);
        }
        drop.insert(drop.end(), std::make_move_iterator(gDropKeep.begin()), std::make_move_iterator(gDropKeep.end()));
        gDropKeep.clear();
        // Only this reference keeps it alive, so only our requeue put it here.
        for (auto const& ref : drop) {
            if (ref.use_count() != 1) continue;
            auto const same = [actor = ref.get()](auto const& item) { return item.get() == actor; };
            std::erase_if(queue, same);
            std::erase_if(alpha, same);
            std::erase_if(shadow, same);
        }
    }
    // The dtor hook locks gAnimMutex.
    drop.clear();
    queueFreshMoving(camera);
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
    // Piston NBT carries no interlock data, so a client-side piston rebuilt from a block-actor packet would
    // stay InitialNotVisible until the engine's timeout and drop the arm for about three ticks.
    if (smoothPistonRenderEnabled()) mTerrainInterlockData->mRenderVisibilityState = VisibilityState::Visible;
    return result;
}

// The round counter has to advance somewhere that runs once per game tick and independently of the render
// framerate.
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

    // Unconditional, because a hidden piston is never handed to the renderer: a fix that lives in the render
    // hook cannot run for the very frames the arm is missing. Tick is the only path that still executes while
    // hidden.
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

    // Same-cell pistons cannot be found by walking the chunk collections - each instance only ever sees itself
    // there - so one head per cell is elected here instead, fresh every round.
    bool const isArm       = entity.mType == ::BlockActorType::PistonArm;
    bool const selfStepped = isArm && hasPistonStepped(&entity);
    // A piston in a moving_block cell is cargo being pushed by another piston rather than the one driving the
    // animation, so it must lose to an actively extending head instead of competing with it.
    bool const isCargo =
        isArm
        && blockEntityRenderData.renderSource.getBlock(entity.mPosition).getTypeName() == "minecraft:moving_block";
    // Driving heads outrank cargo, and within either role a piston that advanced mProgress outranks an
    // untouched leftover. Equal rank falls back to first-come so exactly one head survives.
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

    if (enabled) {
        gFrameLt.store(blockEntityRenderData.renderSource.getLevel().getCurrentTick().tickID);
        gFrameDrewActors.store(true, std::memory_order_relaxed);
    }
    RenderScope const     scope;
    std::optional<::Vec3> bodyOffset;
    if (isArm && animationActive()) bodyOffset = visualBodyOffset(entity.mPosition.get(), renderContext.mFrameAlpha);
    if (!bodyOffset || (bodyOffset->x == 0.0f && bodyOffset->y == 0.0f && bodyOffset->z == 0.0f)) {
        origin(renderContext, blockEntityRenderData);
        return;
    }
    // The dispatcher owns this position for the current draw only; shift the arm with its still-moving body.
    auto&      position  = const_cast<::Vec3&>(blockEntityRenderData.renderPosition);
    auto const saved     = position;
    position.x          += bodyOffset->x;
    position.y          += bodyOffset->y;
    position.z          += bodyOffset->z;
    origin(renderContext, blockEntityRenderData);
    position = saved;
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
    gFrameDrewActors.store(true, std::memory_order_relaxed);

    // A MovingBlock rebuilt mid-animation starts hidden just like the piston does. Only lift that initial
    // state: the retirement logic below owns DelayedDestructionNotVisible and must not be overridden.
    if (interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
    }

    if (animationActive()) {
        registerMoving(*movingPtr, source);
        // A successor or the rebuilt mesh already shows this block; a second copy would be a ghost.
        if (movingRetired(*movingPtr)) return;
        // The visual owns this block until the real block is re-meshed; native retirement would cut it short.
        if (movingHeld(*movingPtr)) {
            interlock.mRenderVisibilityState = VisibilityState::Visible;
            interlock.mHasBeenDelayedDeleted = false;
            origin(renderContext, blockEntityRenderData);
            return;
        }
    }

    auto const& cellBlock = source.getBlock(entity.mPosition);

    // An air-wrapped MovingBlock has no replacement block of its own coming, so hiding it while its cell is
    // still empty would open a hole. Once a real block occupies the cell there is nothing left for it to cover.
    bool const coversNothing = movingPtr->getWrappedBlock().isAir() && cellBlock.isAir();

    // getDrawPos collapses as soon as mProgress reaches 1, but the renderer keeps lerping mLastProgress towards
    // it for another tick. Both fields at 1 is the only state where no interpolation is left.
    auto* const owner   = movingPtr->getOwningPiston(source);
    bool const  arrived = owner != nullptr && owner->mProgress >= 1.0f && owner->mLastProgress >= 1.0f;

    // Detached means the block entity no longer belongs to this cell: the block has been handed to the next
    // MovingBlock one cell along, so this cell is meant to be empty and keeping it drawn only redraws the block
    // at the position it already left.
    bool const detached = source.getBlockEntity(entity.mPosition) != &entity;

    // A piston body never hands its block to a neighbour, so for it detachment only means the cell
    // re-registered a fresh instance. Retiring it left the cell with neither entity nor replacement block for
    // several frames, which is the piston vanishing mid-extension.
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

// A destroyed MovingBlock must leave the tables before its address can be re-queued or reused.
LL_TYPE_INSTANCE_HOOK(
    PlaybackBlockActorDtorHook,
    ll::memory::HookPriority::Normal,
    BlockActor,
    &BlockActor::$dtor,
    void
) {
    if (mType == ::BlockActorType::MovingBlock)
        forgetMoving(static_cast<::MovingBlockActor const*>(static_cast<::BlockActor const*>(this)));
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackStartRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkGeometry,
    &RenderChunkGeometry::startRebuild,
    void,
    ::RenderChunkBuilder& builder,
    ::Vec3 const&         origin_
) {
    meshBuildStarted(this);
    tMeshGeometry = this;
    origin(builder, origin_);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkGeometry,
    &RenderChunkGeometry::rebuild,
    void,
    ::RenderChunkBuilder&                                      builder,
    bool                                                       lightingType,
    ::BakedBlockLightType                                      forExport,
    bool                                                       lightingModelCapabilities,
    ::mce::framebuilder::FrameLightingModelCapabilities const& caps
) {
    auto const saved = tMeshGeometry;
    tMeshGeometry    = this;
    origin(builder, lightingType, forExport, lightingModelCapabilities, caps);
    tMeshGeometry = saved;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackEndRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkGeometry,
    &RenderChunkGeometry::endRebuild,
    void,
    ::RenderChunkBuilder&           builder,
    ::mce::BufferResourceService&   bufferResourceService,
    bool                            isBuilding,
    ::dragon::RenderMetadata const& renderMetadata,
    bool                            useSplitStream
) {
    origin(builder, bufferResourceService, isBuilding, renderMetadata, useSplitStream);
    tMeshGeometry = nullptr;
    meshBuildCommitted(this);
}

// Passes without block actors (export) do not count, so "next frame" always means a frame that drew them.
LL_TYPE_INSTANCE_HOOK(
    PlaybackFrameHook,
    ll::memory::HookPriority::Normal,
    LevelRenderer,
    &LevelRenderer::endFrame,
    void,
    ::mce::TextureResourceService& textureResourceService
) {
    origin(textureResourceService);
    if (gFrameDrewActors.exchange(false, std::memory_order_relaxed)) gFrame.fetch_add(1, std::memory_order_relaxed);
}

// A subchunk re-collect drops a detached MovingBlock from the queue; the main camera overrides the base
// collection.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPlayerQueueEntitiesHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::$queueRenderEntities,
    void,
    ::LevelRenderPreRenderUpdateParameters const& parameters
) {
    origin(parameters);
    requeueHeldMoving(*this);
}

// The camera dispatches by the cell's block; once the real block lands there, a held MovingBlock gets no
// renderer.
std::atomic<::Block const*> gMovingBlockBlock{nullptr};

LL_TYPE_INSTANCE_HOOK(
    PlaybackBlockForEntityHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$_getBlockForBlockEnity,
    ::Block const*,
    ::BlockActor const& blockActor
) {
    auto const* block = origin(blockActor);
    if (blockActor.mType != ::BlockActorType::MovingBlock || !animationActive()) return block;
    if (block && block->getTypeName() == "minecraft:moving_block") {
        gMovingBlockBlock.store(block, std::memory_order_relaxed);
        return block;
    }
    auto const* moving = static_cast<::MovingBlockActor const*>(&blockActor);
    {
        std::lock_guard const guard(gAnimMutex);
        auto const            it = gMovingAction.find(moving);
        if (it == gMovingAction.end() || !heldLocked(it->second)) return block;
    }
    auto const* override_ = gMovingBlockBlock.load(std::memory_order_relaxed);
    return override_ ? override_ : block;
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
    if (airAndSimpleBlocks && touchesLanded(pos)) return origin(tessellator, block, pos, faces, nullptr);
    return origin(tessellator, block, pos, faces, airAndSimpleBlocks);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackFaceOcclusionHook,
    ll::memory::HookPriority::Normal,
    BlockOccluder,
    &BlockOccluder::_shouldRenderFace,
    bool,
    ::BlockPos const& neighborPos,
    uchar             face,
    ::AABB const&     shape,
    ::BlockPos const& pos
) {
    if (origin(neighborPos, face, shape, pos)) return true;
    return neighborHidden(neighborPos) != 0;
}

// A plain area change can sit in the render queue for many frames at high framerates; the hidden block needs it
// now.
LL_TYPE_INSTANCE_HOOK(
    PlaybackImmediateRebuildHook,
    ll::memory::HookPriority::Normal,
    RenderChunkCoordinator,
    &RenderChunkCoordinator::$onAreaChanged,
    void,
    ::BlockSource&    source,
    ::BlockPos const& min,
    ::BlockPos const& max
) {
    origin(source, min, max);
    if (!tImmediateRebuild) return;
    _setDirty(min, max, true, false, false);
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
        bool faceOcclusion{};
        bool immediateRebuild{};
        bool actorDtor{};
        bool startRebuild{};
        bool rebuild{};
        bool endRebuild{};
        bool frame{};
        bool playerQueueEntities{};
        bool blockFor{};
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
        if (!state.faceOcclusion) state.faceOcclusion = PlaybackFaceOcclusionHook::hook() == 0;
        if (!state.faceOcclusion) return false;
        if (!state.immediateRebuild) state.immediateRebuild = PlaybackImmediateRebuildHook::hook() == 0;
        if (!state.immediateRebuild) return false;
        if (!state.actorDtor) state.actorDtor = PlaybackBlockActorDtorHook::hook() == 0;
        if (!state.actorDtor) return false;
        if (!state.startRebuild) state.startRebuild = PlaybackStartRebuildHook::hook() == 0;
        if (!state.startRebuild) return false;
        if (!state.rebuild) state.rebuild = PlaybackRebuildHook::hook() == 0;
        if (!state.rebuild) return false;
        if (!state.endRebuild) state.endRebuild = PlaybackEndRebuildHook::hook() == 0;
        if (!state.endRebuild) return false;
        if (!state.frame) state.frame = PlaybackFrameHook::hook() == 0;
        if (!state.frame) return false;
        if (!state.playerQueueEntities) state.playerQueueEntities = PlaybackPlayerQueueEntitiesHook::hook() == 0;
        if (!state.playerQueueEntities) return false;
        if (!state.blockFor) state.blockFor = PlaybackBlockForEntityHook::hook() == 0;
        if (!state.blockFor) return false;
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
    if (state.blockFor) {
        PlaybackBlockForEntityHook::unhook();
        state.blockFor = false;
    }
    if (state.playerQueueEntities) {
        PlaybackPlayerQueueEntitiesHook::unhook();
        state.playerQueueEntities = false;
    }
    if (state.frame) {
        PlaybackFrameHook::unhook();
        state.frame = false;
    }
    if (state.endRebuild) {
        PlaybackEndRebuildHook::unhook();
        state.endRebuild = false;
    }
    if (state.rebuild) {
        PlaybackRebuildHook::unhook();
        state.rebuild = false;
    }
    if (state.startRebuild) {
        PlaybackStartRebuildHook::unhook();
        state.startRebuild = false;
    }
    if (state.actorDtor) {
        PlaybackBlockActorDtorHook::unhook();
        state.actorDtor = false;
    }
    if (state.immediateRebuild) {
        PlaybackImmediateRebuildHook::unhook();
        state.immediateRebuild = false;
    }
    if (state.faceOcclusion) {
        PlaybackFaceOcclusionHook::unhook();
        state.faceOcclusion = false;
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
