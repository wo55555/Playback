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
#include "mc/world/phys/AABB.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace playback::visuals {

namespace {

using VisibilityState = ::ActorTerrainInterlockData::VisibilityState;

// How long a superseded MovingBlock keeps being drawn after the real block takes over its position. Vanilla holds it
// for 28~30 ticks measured across 13 arrivals, and the destination cell passes through moving_block then air before
// the real block appears ~11 ticks in, so anything shorter than that window leaves a hole. This trims vanilla's hold
// without reaching into the window. Wall time cannot be used: one export frame can exceed a tick, and the actor's own
// mTickCount freezes the moment the real block takes over its position.
constexpr uint64_t ArrivedMovingBlockTailTicks = 18;

// Measured worst case for a server block update to land was 13 ticks; this keeps repairing well past that, then
// gives up so a piston whose blocks were legitimately removed is not resurrected forever.
constexpr uint64_t RestingRepairTickCap = 40;


std::atomic_bool gInstalled{false};

bool smoothPistonRenderEnabled() { return Playback::getInstance().getConfig().smoothPistonRender; }

bool pistonDiagnosticsEnabled() { return Playback::getInstance().getConfig().pistonRenderDiagnostics; }

// One record per MovingBlock, so a single arrival prints its whole visibility history instead of one line per frame.
struct GhostProbe {
    uint64_t    renders{};
    uint64_t    rendersAfterTail{};
    bool        tailStarted{};
    bool        tailExpired{};
    bool        airAtTailStart{};
    bool        airAtTailExpiry{};
    bool        ownerMovingAtTailStart{};
    uint8_t     visibilityAtFirstRender{};
    uint8_t     visibilityAtTailStart{};
    uint8_t     pistonStateAtTailStart{0xFF};
    uint64_t    tickAtTailStart{};
    uint64_t    tickAtTailExpiry{};
    bool        airAtDestination{};
    bool        expandingAtTailStart{};
    bool        wrappedIsAir{};
    uint64_t    forcedVisible{};
    ::BlockPos  destination{};
    std::string wrappedName;
    std::string existingName;
    // One entry per rendered frame. A gap the eye can see must fall between two of these, or before the first one.
    std::string timeline;
    uint64_t    progressRestored{};
    uint64_t    heldTicks{};
    // Carried from the sampling block to the timeline write, which has to happen after the override is applied.
    uint64_t frameTick{};
    bool     frameCellAir{};
    bool     frameDetached{};
    uint8_t  frameVisibility{};
    uint64_t firstRenderTick{};
    uint64_t lastRenderTick{};
    uint64_t detachedFirstSeenTick{};
    uint64_t blockLandedTick{};
    bool     everDetached{};
    bool     everLanded{};
};

// Keyed by actor address; MovingBlocks are short-lived so the map is pruned whenever an entry reaches its verdict.
std::unordered_map<::MovingBlockActor const*, GhostProbe> gGhostProbes;

// Tail start tick per arrived MovingBlock; the interlock struct has no field we can reuse for a tick stamp.
std::unordered_map<::MovingBlockActor const*, uint64_t> gTailAnchors;


struct BlockPosHash {
    [[nodiscard]] size_t operator()(::BlockPos const& pos) const noexcept {
        return (static_cast<size_t>(static_cast<uint32_t>(pos.x)) * 73856093u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.y)) * 19349663u)
             ^ (static_cast<size_t>(static_cast<uint32_t>(pos.z)) * 83492791u);
    }
};

// Blocks captured per piston while the attached list is still valid, keyed by their pre-move position.
std::unordered_map<::PistonBlockActor const*, std::unordered_map<::BlockPos, ::Block const*, BlockPosHash>>
    gAttachedBlockCache;

// Destination cells a piston owes a real block, captured while mAttachedBlocks is still populated. The edge where
// the animation ends can be missed entirely, so arrival is re-verified every resting tick instead of once.
struct PendingArrival {
    std::unordered_map<::BlockPos, ::Block const*, BlockPosHash> cells;
    uint64_t                                                     sinceTick{};
};

std::unordered_map<::PistonBlockActor const*, PendingArrival> gPendingArrivals;


// Last seen arm visibility per piston, so only transitions are logged instead of one line per frame.
std::unordered_map<::BlockActor const*, uint8_t> gArmStates;

// Cells that hit the tick cap while still air, kept so the next frames can report when the real block finally lands.
// Only diagnostics: distinguishes a block update that is merely late from one the server never sends.
std::unordered_map<::BlockPos, uint64_t, BlockPosHash> gCappedCells;

// Sentinel means "no export running"; renders outside an export are not worth a CSV row because nothing links them to
// an image the user can look at.
constexpr uint64_t NoExportFrame = UINT64_MAX;

std::atomic<uint64_t> gDiagnosticsFrame{NoExportFrame};

// Diagnostics also go to a file next to the mod: during export the console scrolls past and is easy to lose.
void appendGhostTrace(std::string const& line) {
    static std::mutex     mutex;
    std::lock_guard const guard(mutex);
    static std::ofstream  file(Playback::getInstance().getSelf().getModDir() / "piston-diag.log", std::ios::app);
    if (file) file << line << '\n' << std::flush;
}

// One row per MovingBlock draw, keyed by the export frame index so a frame_%06d.png can be looked up directly. The
// per-entity verdict summaries cannot answer "what did frame N contain", which is the only question worth asking
// about a visual glitch.
void appendFrameRow(std::string const& line) {
    static std::mutex     mutex;
    std::lock_guard const guard(mutex);
    static std::ofstream  file(Playback::getInstance().getSelf().getModDir() / "piston-frames.csv", std::ios::app);
    static bool const     header = [] {
        std::ofstream head(Playback::getInstance().getSelf().getModDir() / "piston-frames.csv", std::ios::trunc);
        if (head) {
            head << "frame,tick,entity,pos,wrapped,wrappedAir,cellBlock,cellAir,detached,visibility,hidden,"
                    "progress,lastProgress,frameAlpha,tailAnchor,heldTicks,expanding,pistonState,pistonMoving\n";
        }
        return true;
    }();
    (void)header;
    if (file) file << line << '\n';
}

// Diagnostics are gated behind a config flag, so info level keeps them visible in release builds.
void logGhostVerdict(::MovingBlockActor const& actor, GhostProbe const& probe, char const* reason) {
    auto const line = fmt::format(
        "[PistonDiag] mb={} verdict={} renders={} rendersAfterTail={} tailStarted={} tailExpired={} "
        "visFirst={} visAtTailStart={} airAtTailStart={} airAtTailExpiry={} pistonStateAtTailStart={} "
        "ownerMovingAtTailStart={} tickAtTailStart={} tickAtTailExpiry={} airAtDestination={} expanding={} "
        "dest=({},{},{}) wrapped={} existing={} wrappedAir={} forcedVisible={} "
        "firstRender={} lastRender={} detachedFirstSeen={} landed={} everDetached={} everLanded={} "
        "progressRestored={} heldTicks={}\n"
        "[PistonDiag]   timeline mb={} {}",
        static_cast<void const*>(&actor),
        reason,
        probe.renders,
        probe.rendersAfterTail,
        probe.tailStarted,
        probe.tailExpired,
        probe.visibilityAtFirstRender,
        probe.visibilityAtTailStart,
        probe.airAtTailStart,
        probe.airAtTailExpiry,
        probe.pistonStateAtTailStart,
        probe.ownerMovingAtTailStart,
        probe.tickAtTailStart,
        probe.tickAtTailExpiry,
        probe.airAtDestination,
        probe.expandingAtTailStart,
        probe.destination.x,
        probe.destination.y,
        probe.destination.z,
        probe.wrappedName,
        probe.existingName,
        probe.wrappedIsAir,
        probe.forcedVisible,
        probe.firstRenderTick,
        probe.lastRenderTick,
        probe.detachedFirstSeenTick,
        probe.blockLandedTick,
        probe.everDetached,
        probe.everLanded,
        probe.progressRestored,
        probe.heldTicks,
        static_cast<void const*>(&actor),
        probe.timeline
    );
    Playback::getInstance().getSelf().getLogger().info("{}", line);
    appendGhostTrace(line);
}

// Measures how often this runs per world tick, to find out whether it can pace the animation. PistonBlockActor::tick
// turned out to fire only every 6 ticks, so no call site may be assumed to be per-tick without measuring it.
LL_TYPE_INSTANCE_HOOK(
    PlaybackMovingBlockTickProbeHook,
    ll::memory::HookPriority::Lowest,
    MovingBlockActor,
    &MovingBlockActor::$tick,
    void,
    ::BlockSource& region
) {
    origin(region);
    if (!smoothPistonRenderEnabled() || !pistonDiagnosticsEnabled()) return;
    if (!region.getLevel().isClientSide()) return;

    static std::unordered_map<::MovingBlockActor const*, uint64_t> lastTick;
    auto const                                                     nowTick = region.getLevel().getCurrentTick().tickID;
    auto&                                                          slot    = lastTick[this];
    appendGhostTrace(
        fmt::format(
            "[PistonDiag] mbTick mb={} tick={} sinceLast={}",
            static_cast<void const*>(this),
            nowTick,
            slot ? nowTick - slot : 0
        )
    );
    slot = nowTick;
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
    gAttachedBlockCache.erase(static_cast<::PistonBlockActor const*>(this));
    gPendingArrivals.erase(static_cast<::PistonBlockActor const*>(this));
    // Piston NBT carries no interlock data, so a client-side piston rebuilt from a block-actor packet would stay
    // InitialNotVisible until the engine's timeout and drop the arm for about three ticks.
    if (smoothPistonRenderEnabled()) mTerrainInterlockData->mRenderVisibilityState = VisibilityState::Visible;
    if (pistonDiagnosticsEnabled()) {
        appendGhostTrace(
            fmt::format(
                "[PistonDiag] pistonCtor piston={} pos=({},{},{}) sticky={}",
                static_cast<void const*>(this),
                pos.x,
                pos.y,
                pos.z,
                isSticky
            )
        );
    }
    return result;
}

// Observation only. The article's account of the arm disappearing is client-side: the client rebuilds the piston
// block entity from the packet NBT, which carries no interlock data. These two probes show whether either NBT path
// leaves the entity at InitialNotVisible after the engine is done with it.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonLoadProbeHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$load,
    void,
    ::ILevel&            level,
    ::CompoundTag const& tag,
    ::DataLoadHelper&    dataLoadHelper
) {
    auto const before = static_cast<uint8_t>(mTerrainInterlockData->mRenderVisibilityState);
    origin(level, tag, dataLoadHelper);
    if (!pistonDiagnosticsEnabled()) return;
    auto const& pos = mPosition.get();
    appendGhostTrace(
        fmt::format(
            "[PistonDiag] pistonLoad piston={} pos=({},{},{}) before={} after={} deleted={} state={}",
            static_cast<void const*>(this),
            pos.x,
            pos.y,
            pos.z,
            before,
            static_cast<uint8_t>(mTerrainInterlockData->mRenderVisibilityState),
            mTerrainInterlockData->mHasBeenDelayedDeleted,
            static_cast<int>(mState)
        )
    );
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonUpdateProbeHook,
    ll::memory::HookPriority::Lowest,
    PistonBlockActor,
    &PistonBlockActor::$_onUpdatePacket,
    void,
    ::CompoundTag const& data,
    ::BlockSource&       region
) {
    auto const before = static_cast<uint8_t>(mTerrainInterlockData->mRenderVisibilityState);
    origin(data, region);
    if (!pistonDiagnosticsEnabled()) return;
    auto const& pos = mPosition.get();
    appendGhostTrace(
        fmt::format(
            "[PistonDiag] pistonPacket piston={} pos=({},{},{}) before={} after={} deleted={} state={} tick={}",
            static_cast<void const*>(this),
            pos.x,
            pos.y,
            pos.z,
            before,
            static_cast<uint8_t>(mTerrainInterlockData->mRenderVisibilityState),
            mTerrainInterlockData->mHasBeenDelayedDeleted,
            static_cast<int>(mState),
            region.getLevel().getCurrentTick().tickID
        )
    );
}

// The engine waits for a server block update before the real block appears, leaving a hole where the MovingBlock
// used to be. Spawning the blocks as soon as the animation ends closes that hole without touching the server state.
// Retiring the MovingBlocks here too would hide them before the spawn lands, so that is left to the render tail.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonArrivalHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$tick,
    void,
    ::BlockSource& region
) {
    auto const wasMoving = isMoving();
    // Captured before origin() clears it: the list is only populated while the piston is actually moving.
    auto const attached = wasMoving ? mAttachedBlocks->size() : 0u;

    origin(region);

    if (!smoothPistonRenderEnabled()) return;

    if (pistonDiagnosticsEnabled() && !gCappedCells.empty()) {
        auto const nowTick = region.getLevel().getCurrentTick().tickID;
        for (auto cellIt = gCappedCells.begin(); cellIt != gCappedCells.end();) {
            auto const& cell   = cellIt->first;
            auto const  waited = nowTick - cellIt->second;
            bool const  landed = !region.getBlock(cell).isAir();
            if (landed || waited >= 200) {
                appendGhostTrace(
                    fmt::format(
                        "[PistonDiag] capped{} cell=({},{},{}) block={} waited={}",
                        landed ? "Landed" : "NeverLanded",
                        cell.x,
                        cell.y,
                        cell.z,
                        region.getBlock(cell).getTypeName(),
                        waited
                    )
                );
                cellIt = gCappedCells.erase(cellIt);
            } else {
                ++cellIt;
            }
        }
    }

    // Unconditional, because a hidden piston is never handed to the renderer: a fix that lives in the render hook
    // cannot run for the very frames the arm is missing. Tick is the only path that still executes while hidden.
    auto& interlock = mTerrainInterlockData.get();
    if (interlock.mRenderVisibilityState != VisibilityState::Visible || interlock.mHasBeenDelayedDeleted) {
        if (pistonDiagnosticsEnabled()) {
            auto const& pos = mPosition.get();
            appendGhostTrace(
                fmt::format(
                    "[PistonDiag] armTick piston={} pos=({},{},{}) vis={} deleted={} state={} moving={}",
                    static_cast<void const*>(this),
                    pos.x,
                    pos.y,
                    pos.z,
                    static_cast<int>(interlock.mRenderVisibilityState),
                    interlock.mHasBeenDelayedDeleted,
                    static_cast<int>(mState),
                    isMoving()
                )
            );
        }
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        interlock.mHasBeenDelayedDeleted = false;
    }

    if (wasMoving && !isMoving() && attached != 0) {
        _spawnBlocks(region);
        if (pistonDiagnosticsEnabled()) {
            appendGhostTrace(
                fmt::format(
                    "[PistonDiag] arrival piston={} state={} attached={}",
                    static_cast<void const*>(this),
                    static_cast<int>(mState),
                    attached
                )
            );
        }
    }

    // Every resting tick, not just the arrival edge: the edge above depends on observing the exact tick isMoving()
    // drops, and a packet that jumps straight to a resting state never produces it. Re-spawning while any owed cell
    // is still air is what makes this self-correcting, mirroring Sapphire's resting-state branches.
    if (isMoving()) return;

    auto const pendingIt = gPendingArrivals.find(this);
    if (pendingIt == gPendingArrivals.end()) return;

    auto&      pending = pendingIt->second;
    auto const nowTick = region.getLevel().getCurrentTick().tickID;

    size_t missing = 0;
    for (auto const& [cell, block] : pending.cells) {
        if (region.getBlock(cell).isAir()) ++missing;
    }

    if (missing == 0 || nowTick - pending.sinceTick > RestingRepairTickCap) {
        if (pistonDiagnosticsEnabled()) {
            appendGhostTrace(
                fmt::format(
                    "[PistonDiag] restingDone piston={} owed={} missing={} waited={}",
                    static_cast<void const*>(this),
                    pending.cells.size(),
                    missing,
                    nowTick - pending.sinceTick
                )
            );
        }
        gPendingArrivals.erase(pendingIt);
        return;
    }

    _spawnBlocks(region);

    size_t stillMissing = 0;
    for (auto const& [cell, block] : pending.cells) {
        if (region.getBlock(cell).isAir()) ++stillMissing;
    }

    if (pistonDiagnosticsEnabled()) {
        appendGhostTrace(
            fmt::format(
                "[PistonDiag] restingRepair piston={} state={} owed={} missingBefore={} missingAfter={} waited={}",
                static_cast<void const*>(this),
                static_cast<int>(mState),
                pending.cells.size(),
                missing,
                stillMissing,
                nowTick - pending.sinceTick
            )
        );
    }
}

// Caches the block at each attached position while mAttachedBlocks is still populated. The engine can lose the
// original block between receiving the piston state packet and building the MovingBlock, which is what makes a
// pushed block briefly render as nothing.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonCacheHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::$_onUpdatePacket,
    void,
    ::CompoundTag const& data,
    ::BlockSource&       region
) {
    auto const previousState = mState;
    origin(data, region);
    if (!smoothPistonRenderEnabled()) return;
    if (previousState == mState || !(mState == ::PistonState::Expanding || mState == ::PistonState::Retracting)) return;

    auto& cache = gAttachedBlockCache[this];
    cache.clear();
    cache.reserve(mAttachedBlocks->size());

    auto&      pending   = gPendingArrivals[this];
    auto const facing    = getFacingDir(region);
    bool const expanding = mState == ::PistonState::Expanding;
    pending.cells.clear();
    pending.sinceTick = region.getLevel().getCurrentTick().tickID;

    for (auto const& pos : mAttachedBlocks.get()) {
        auto const*    actor   = region.getBlockEntity(pos);
        ::Block const* wrapped = nullptr;
        if (actor && actor->mType == ::BlockActorType::MovingBlock) {
            wrapped = &static_cast<::MovingBlockActor const*>(actor)->getWrappedBlock();
        } else {
            wrapped = &region.getBlock(pos);
        }
        cache.emplace(pos, wrapped);

        auto const destination = expanding ? ::BlockPos{pos.x + facing.x, pos.y + facing.y, pos.z + facing.z}
                                           : ::BlockPos{pos.x - facing.x, pos.y - facing.y, pos.z - facing.z};
        if (wrapped && !wrapped->isAir()) pending.cells.emplace(destination, wrapped);
    }

    if (pistonDiagnosticsEnabled()) {
        appendGhostTrace(
            fmt::format(
                "[PistonDiag] cache piston={} state={} entries={} pending={} tick={}",
                static_cast<void const*>(this),
                static_cast<int>(mState),
                cache.size(),
                pending.cells.size(),
                pending.sinceTick
            )
        );
    }
}

// Restores the wrapped block from the cache right after the engine builds the MovingBlock.
LL_TYPE_INSTANCE_HOOK(
    PlaybackSpawnMovingBlockHook,
    ll::memory::HookPriority::Normal,
    PistonBlockActor,
    &PistonBlockActor::_spawnMovingBlock,
    void,
    ::BlockSource&    region,
    ::BlockPos const& blockPos
) {
    origin(region, blockPos);
    if (!smoothPistonRenderEnabled()) return;

    auto const& facing      = getFacingDir(region);
    bool const  expanding   = mState == ::PistonState::Expanding || mState == ::PistonState::Expanded;
    auto const  destination = expanding
                                ? ::BlockPos{blockPos.x + facing.x, blockPos.y + facing.y, blockPos.z + facing.z}
                                : ::BlockPos{blockPos.x - facing.x, blockPos.y - facing.y, blockPos.z - facing.z};

    auto const cacheIt = gAttachedBlockCache.find(this);
    if (cacheIt == gAttachedBlockCache.end()) return;
    auto const entryIt = cacheIt->second.find(blockPos);
    if (entryIt == cacheIt->second.end() || !entryIt->second) return;

    auto* actor = region.getBlockEntity(destination);
    if (!actor || actor->mType != ::BlockActorType::MovingBlock) return;

    auto* moving = static_cast<::MovingBlockActor*>(actor);
    if (!moving->getWrappedBlock().isAir()) return;

    moving->setWrappedBlock(*entryIt->second);
    if (pistonDiagnosticsEnabled()) {
        appendGhostTrace(
            fmt::format(
                "[PistonDiag] repaired mb={} from=({},{},{}) to=({},{},{}) block={}",
                static_cast<void const*>(moving),
                blockPos.x,
                blockPos.y,
                blockPos.z,
                destination.x,
                destination.y,
                destination.z,
                entryIt->second->getTypeName()
            )
        );
    }
}

// The same piston position is torn down and rebuilt many times per replay, and every fresh instance starts at
// InitialNotVisible. Forcing visibility here covers every construction path, since nothing reaches the screen
// without passing through the renderer.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPistonArmProbeHook,
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
    auto const before  = static_cast<uint8_t>(interlock.mRenderVisibilityState);
    bool const hidden  = interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible;
    bool const deleted = interlock.mHasBeenDelayedDeleted;

    if (enabled && hidden) {
        interlock.mRenderVisibilityState = VisibilityState::Visible;
        // A rebuilt piston also inherits the delete flag, which would re-hide it on the next frame.
        interlock.mHasBeenDelayedDeleted = false;
    }

    if (pistonDiagnosticsEnabled()) {
        auto&      seen  = gArmStates[&entity];
        auto const after = static_cast<uint8_t>(interlock.mRenderVisibilityState);
        if (seen != after || (hidden && enabled)) {
            auto const& pos = entity.mPosition.get();
            appendGhostTrace(
                fmt::format(
                    "[PistonDiag] arm piston={} pos=({},{},{}) type={} before={} after={} forced={} wasDeleted={} "
                    "enabled={}",
                    static_cast<void const*>(&entity),
                    pos.x,
                    pos.y,
                    pos.z,
                    static_cast<int>(entity.mType),
                    before,
                    after,
                    hidden && enabled,
                    deleted,
                    enabled
                )
            );
            seen = after;
        }

        auto const exportFrame = gDiagnosticsFrame.load(std::memory_order_relaxed);
        if (exportFrame != NoExportFrame) {
            auto const* const piston =
                entity.mType == ::BlockActorType::PistonArm ? static_cast<::PistonBlockActor const*>(&entity) : nullptr;
            auto const& pos        = entity.mPosition.get();
            auto const  visibility = static_cast<uint8_t>(interlock.mRenderVisibilityState);
            appendFrameRow(
                fmt::format(
                    "{},{},{},\"{},{},{}\",piston,0,piston,0,0,{},{},{:.3f},{:.3f},{:.3f},-1,-1,0,{},{}",
                    exportFrame,
                    blockEntityRenderData.renderSource.getLevel().getCurrentTick().tickID,
                    static_cast<void const*>(&entity),
                    pos.x,
                    pos.y,
                    pos.z,
                    visibility,
                    visibility == static_cast<uint8_t>(VisibilityState::DelayedDestructionNotVisible) ? 1 : 0,
                    piston ? piston->mProgress : -1.0f,
                    piston ? piston->mLastProgress : -1.0f,
                    renderContext.mFrameAlpha,
                    piston ? static_cast<int>(piston->mState) : -1,
                    piston && piston->isMoving() ? 1 : 0
                )
            );
        }
    }
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
    bool const diagnose  = isMoving && pistonDiagnosticsEnabled();
    auto*      movingPtr = isMoving ? static_cast<::MovingBlockActor*>(&entity) : nullptr;

    // Separates "hook never ran" from "hook ran but saw no MovingBlock"; without it an empty trace is ambiguous.
    if (pistonDiagnosticsEnabled()) {
        static std::atomic_bool firstHit{false};
        if (!firstHit.exchange(true)) {
            appendGhostTrace(fmt::format("[PistonDiag] first render hit type={}", static_cast<int>(entity.mType)));
        }
    }

    GhostProbe* probe = nullptr;
    if (diagnose) {
        auto& slot = gGhostProbes[movingPtr];
        probe      = &slot;
        if (probe->renders == 0) {
            probe->visibilityAtFirstRender = static_cast<uint8_t>(entity.mTerrainInterlockData->mRenderVisibilityState);
        }
        ++probe->renders;
        if (probe->tailStarted) ++probe->rendersAfterTail;

        auto&       timelineSource = blockEntityRenderData.renderSource;
        auto const  frameTick      = timelineSource.getLevel().getCurrentTick().tickID;
        auto const& selfCell       = entity.mPosition.get();
        bool const  cellAir        = timelineSource.getBlock(selfCell).isAir();
        bool const  detached       = timelineSource.getBlockEntity(entity.mPosition) != &entity;
        auto const  visibility     = static_cast<uint8_t>(entity.mTerrainInterlockData->mRenderVisibilityState);

        if (probe->renders == 1) probe->firstRenderTick = frameTick;
        probe->lastRenderTick = frameTick;
        if (detached && !probe->everDetached) {
            probe->everDetached          = true;
            probe->detachedFirstSeenTick = frameTick;
        }
        if (!cellAir && !probe->everLanded) {
            probe->everLanded      = true;
            probe->blockLandedTick = frameTick;
        }
        probe->frameTick       = frameTick;
        probe->frameCellAir    = cellAir;
        probe->frameDetached   = detached;
        probe->frameVisibility = visibility;
    }

    // Written before the control-group return so the disabled run still records how long vanilla keeps drawing each
    // MovingBlock; that window is the only reference for how long the tail should be.
    if (isMoving && pistonDiagnosticsEnabled()) {
        auto const exportFrame = gDiagnosticsFrame.load(std::memory_order_relaxed);
        if (exportFrame != NoExportFrame) {
            auto&             source     = blockEntityRenderData.renderSource;
            auto const&       interlock  = entity.mTerrainInterlockData.get();
            auto const&       cell       = entity.mPosition.get();
            auto const&       cellBlock  = source.getBlock(cell);
            auto const* const owner      = movingPtr->getOwningPiston(source);
            auto const        nowTick    = source.getLevel().getCurrentTick().tickID;
            auto const        anchorIt   = gTailAnchors.find(movingPtr);
            bool const        hasAnchor  = anchorIt != gTailAnchors.end();
            auto const        visibility = static_cast<uint8_t>(interlock.mRenderVisibilityState);
            appendFrameRow(
                fmt::format(
                    "{},{},{},\"{},{},{}\",{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{},{},{},{},{}",
                    exportFrame,
                    nowTick,
                    static_cast<void const*>(movingPtr),
                    cell.x,
                    cell.y,
                    cell.z,
                    movingPtr->getWrappedBlock().getTypeName(),
                    movingPtr->getWrappedBlock().isAir() ? 1 : 0,
                    cellBlock.getTypeName(),
                    cellBlock.isAir() ? 1 : 0,
                    source.getBlockEntity(entity.mPosition) != &entity ? 1 : 0,
                    visibility,
                    visibility == static_cast<uint8_t>(VisibilityState::DelayedDestructionNotVisible) ? 1 : 0,
                    owner ? owner->mProgress : -1.0f,
                    owner ? owner->mLastProgress : -1.0f,
                    renderContext.mFrameAlpha,
                    hasAnchor ? static_cast<int64_t>(anchorIt->second) : -1,
                    hasAnchor ? static_cast<int64_t>(nowTick - anchorIt->second) : -1,
                    movingPtr->mPistonBlockExpanding ? 1 : 0,
                    owner ? static_cast<int>(owner->mState) : -1,
                    owner && owner->isMoving() ? 1 : 0
                )
            );
        }
    }

    if (!smoothPistonRenderEnabled()) {
        // Control group: the native tail stays in place, so a gap that survives here is not ours.
        if (probe && probe->renders == 1) logGhostVerdict(*movingPtr, *probe, "control-disabled");
        return origin(renderContext, blockEntityRenderData);
    }

    // Progress is left exactly as the piston reports it. Forcing it to 1.0 whenever the piston was not moving drew
    // mid-animation MovingBlocks at their destination instead of along the path, which read as a trailing ghost and
    // as the travelling block never appearing. Sapphire likewise never touches progress in its smoothing fix.
    if (isMoving && probe && probe->timeline.size() < 6000) {
        auto&       source   = blockEntityRenderData.renderSource;
        auto* const owner    = movingPtr->getOwningPiston(source);
        float const current  = owner ? owner->mProgress : -1.0f;
        float const previous = owner ? owner->mLastProgress : -1.0f;
        // mProgress only ever holds 0/0.5/1; the smooth position comes from lerping it against mLastProgress by the
        // frame alpha, so all three have to be sampled to tell which one is failing to advance.
        probe->timeline += fmt::format(
            "{}t{}/a{}/d{}/v{}/p{:.2f}/l{:.2f}/f{:.3f}",
            probe->timeline.empty() ? "" : " ",
            probe->frameTick,
            probe->frameCellAir ? 1 : 0,
            probe->frameDetached ? 1 : 0,
            probe->frameVisibility,
            current,
            previous,
            renderContext.mFrameAlpha
        );
    }

    if (isMoving) {
        auto&      interlock  = entity.mTerrainInterlockData.get();
        auto&      source     = blockEntityRenderData.renderSource;
        auto const nowTick    = source.getLevel().getCurrentTick().tickID;
        auto&      tailAnchor = gTailAnchors;
        // A MovingBlock rebuilt mid-animation starts hidden just like the piston does. Only lift that initial state:
        // the tail logic below owns DelayedDestructionNotVisible and must not be overridden.
        if (interlock.mRenderVisibilityState == VisibilityState::InitialNotVisible) {
            interlock.mRenderVisibilityState = VisibilityState::Visible;
            if (probe) ++probe->forcedVisible;
        }
        // A MovingBlock wrapping air carries nothing, so no real block will ever land in its cell and the tail below
        // would run to its cap while drawing a hole in the middle of the column.
        bool const carriesNothing = movingPtr->getWrappedBlock().isAir();

        if (carriesNothing) {
            // Leave it to vanilla: nothing to hold on screen, nothing to hide.
        } else if (auto const it = tailAnchor.find(movingPtr); it != tailAnchor.end()) {
            // Elapsed ticks only. A real block present in the destination cell does not mean the chunk mesh has drawn
            // it yet, so cell contents can never end the tail.
            if (nowTick - it->second >= ArrivedMovingBlockTailTicks) {
                interlock.mRenderVisibilityState = VisibilityState::DelayedDestructionNotVisible;
            }
        } else if (source.getBlockEntity(entity.mPosition) != &entity) {
            // The real block has taken over this position, so start the shortened tail from now.
            interlock.mHasBeenDelayedDeleted = true;
            tailAnchor[movingPtr]            = nowTick;
        }
    }

    origin(renderContext, blockEntityRenderData);
}

// Candidate B: if the client rebuilds MovingBlocks from NBT too, the gap is at departure, not arrival.
LL_TYPE_INSTANCE_HOOK(
    PlaybackMovingBlockConstructionProbeHook,
    ll::memory::HookPriority::Normal,
    MovingBlockActor,
    &MovingBlockActor::$ctor,
    void*,
    ::BlockPos const& pos
) {
    auto* result = origin(pos);
    // A recycled address must not inherit the previous MovingBlock's tail or probe state. Flush the old timeline
    // first: the frames after the tail expired are exactly the ones a per-verdict log never gets to print.
    auto* const self = static_cast<::MovingBlockActor*>(this);
    if (pistonDiagnosticsEnabled()) {
        if (auto const it = gGhostProbes.find(self); it != gGhostProbes.end() && it->second.renders > 0) {
            logGhostVerdict(*self, it->second, "recycled");
        }
    }
    gTailAnchors.erase(static_cast<::MovingBlockActor const*>(this));
    gGhostProbes.erase(static_cast<::MovingBlockActor const*>(this));
    if (pistonDiagnosticsEnabled()) {
        appendGhostTrace(
            fmt::format(
                "[PistonDiag] mb={} constructed visibility={} delayedDeleted={}",
                static_cast<void const*>(this),
                static_cast<uint8_t>(mTerrainInterlockData->mRenderVisibilityState),
                mTerrainInterlockData->mHasBeenDelayedDeleted
            )
        );
    }
    return result;
}

} // namespace

bool hookPistonRender(bool enable) {
    struct HookState {
        bool arm{};
        bool armProbe{};
        bool arrival{};
        bool cache{};
        bool spawn{};
        bool ghost{};
        bool movingCtor{};
        bool loadProbe{};
        bool packetProbe{};
        bool mbTickProbe{};
    };
    static HookState state;

    auto installAll = [&] {
        // Left uninstalled: PistonBlockActor::tick only runs every 6 world ticks in 26.20, so a phase machine driven
        // from it cannot spread the animation and strands MovingBlocks between phases.
        if (!state.arm) state.arm = PlaybackPistonArmVisibilityHook::hook() == 0;
        if (!state.arm) return false;
        if (!state.armProbe) state.armProbe = PlaybackPistonArmProbeHook::hook() == 0;
        if (!state.armProbe) return false;
        if (!state.loadProbe) state.loadProbe = PlaybackPistonLoadProbeHook::hook() == 0;
        if (!state.loadProbe) return false;
        if (!state.packetProbe) state.packetProbe = PlaybackPistonUpdateProbeHook::hook() == 0;
        if (!state.packetProbe) return false;
        if (!state.cache) state.cache = PlaybackPistonCacheHook::hook() == 0;
        if (!state.cache) return false;
        if (!state.spawn) state.spawn = PlaybackSpawnMovingBlockHook::hook() == 0;
        if (!state.spawn) return false;
        if (!state.arrival) state.arrival = PlaybackPistonArrivalHook::hook() == 0;
        if (!state.arrival) return false;
        if (!state.ghost) state.ghost = PlaybackMovingBlockGhostHook::hook() == 0;
        if (!state.ghost) return false;
        if (!state.movingCtor) state.movingCtor = PlaybackMovingBlockConstructionProbeHook::hook() == 0;
        if (!state.movingCtor) return false;
        if (!state.mbTickProbe) state.mbTickProbe = PlaybackMovingBlockTickProbeHook::hook() == 0;
        return state.mbTickProbe;
    };
    auto removeAll = [&] {
        if (state.mbTickProbe && PlaybackMovingBlockTickProbeHook::unhook()) state.mbTickProbe = false;
        if (state.movingCtor && PlaybackMovingBlockConstructionProbeHook::unhook()) state.movingCtor = false;
        if (state.ghost && PlaybackMovingBlockGhostHook::unhook()) state.ghost = false;
        if (state.arrival && PlaybackPistonArrivalHook::unhook()) state.arrival = false;
        if (state.spawn && PlaybackSpawnMovingBlockHook::unhook()) state.spawn = false;
        if (state.cache && PlaybackPistonCacheHook::unhook()) state.cache = false;
        if (state.packetProbe && PlaybackPistonUpdateProbeHook::unhook()) state.packetProbe = false;
        if (state.loadProbe && PlaybackPistonLoadProbeHook::unhook()) state.loadProbe = false;
        if (state.armProbe && PlaybackPistonArmProbeHook::unhook()) state.armProbe = false;
        if (state.arm && PlaybackPistonArmVisibilityHook::unhook()) state.arm = false;
        return !state.arm && !state.armProbe && !state.cache && !state.spawn && !state.arrival && !state.ghost
            && !state.movingCtor && !state.loadProbe && !state.packetProbe && !state.mbTickProbe;
    };

    auto allInstalled = [&] {
        return state.arm && state.armProbe && state.cache && state.spawn && state.arrival && state.ghost
            && state.movingCtor && state.loadProbe && state.packetProbe && state.mbTickProbe;
    };
    auto noneInstalled = [&] {
        return !state.arm && !state.armProbe && !state.cache && !state.spawn && !state.arrival && !state.ghost
            && !state.movingCtor && !state.loadProbe && !state.packetProbe && !state.mbTickProbe;
    };

    if (enable) {
        bool const installed = allInstalled() || installAll();
        if (!installed) (void)removeAll();
        gInstalled.store(installed, std::memory_order_release);
        // Written unconditionally so an empty trace file can never be mistaken for "no MovingBlocks rendered".
        appendGhostTrace(
            fmt::format(
                "[PistonDiag] install arm={} armProbe={} cache={} spawn={} arrival={} ghost={} movingCtor={} "
                "loadProbe={} packetProbe={} mbTickProbe={} diagnostics={}",
                state.arm,
                state.armProbe,
                state.cache,
                state.spawn,
                state.arrival,
                state.ghost,
                state.movingCtor,
                state.loadProbe,
                state.packetProbe,
                state.mbTickProbe,
                pistonDiagnosticsEnabled()
            )
        );
        return installed;
    }

    bool const removed = noneInstalled() || removeAll();
    if (removed) {
        gGhostProbes.clear();
        gTailAnchors.clear();
        gAttachedBlockCache.clear();
        gPendingArrivals.clear();
        gCappedCells.clear();
        gArmStates.clear();
    }
    gInstalled.store(!removed && installAll(), std::memory_order_release);
    return removed;
}

bool isPistonRenderInstalled() noexcept { return gInstalled.load(std::memory_order_acquire); }

void setPistonDiagnosticsFrameIndex(uint64_t frameIndex) noexcept {
    gDiagnosticsFrame.store(frameIndex, std::memory_order_relaxed);
}

void clearPistonDiagnosticsFrameIndex() noexcept { gDiagnosticsFrame.store(NoExportFrame, std::memory_order_relaxed); }

} // namespace playback::visuals
