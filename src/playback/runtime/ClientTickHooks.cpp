#include "ClientTickHooks.h"
#include "WindowCloseGuard.h"

#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/editor/graphics/ReplayMouseHook.h"
#include "playback/exporting/ExportEcoMode.h"
#include "playback/exporting/OfflineRenderClockHooks.h"
#include "playback/record/ChunkMutationBarrier.h"
#include "playback/record/Recorder.h"
#include "playback/replay/ReplaySession.h"
#include "playback/screen/RecordDialogs.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/SceneType.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/world/level/storage/LevelData.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace playback::runtime {
using namespace playback::record;
using namespace playback::replay;

namespace {

struct OfflineTickGateState {
    bool                                       active{};
    uint64_t                                   nextTokenId{1};
    std::optional<uint64_t>                    pendingToken;
    std::optional<uint64_t>                    inFlightToken;
    std::optional<uint64_t>                    completedToken;
    std::optional<OfflineReplayTickCompletion> completion;
    std::chrono::steady_clock::time_point      requestedAt;
    OfflineReplayTickPacing                    pacing;
};

struct OfflineTickClaim {
    bool                    controlled{};
    std::optional<uint64_t> token;
};

std::atomic_bool     gClientTickHooksInstalled{false};
std::mutex           gOfflineTickGateMutex;
OfflineTickGateState gOfflineTickGate;
// Replay ticks an export has run that still owe the tick the game pairs with each tick of its real-time timer.
std::atomic<int>  gOfflineRealTicksOwed{0};
bool              gOfflineRealTickHooked{false};
thread_local bool tOfflineRealTick{false};
// The game makes that call on the client's GameCallbacks base, not on the client itself, so the export makes its own
// calls on the pointer the game last used.
std::atomic<ClientInstance*> gOfflineRealTickTarget{nullptr};

// Set while the replay tick runs after the native level tick, so a seek can tick the level in step with its ticks.
thread_local std::function<void()> gNativeLevelTick;

struct NativeLevelTickScope {
    explicit NativeLevelTickScope(std::function<void()> tick) { gNativeLevelTick = std::move(tick); }
    ~NativeLevelTickScope() { gNativeLevelTick = nullptr; }
    NativeLevelTickScope(NativeLevelTickScope const&)            = delete;
    NativeLevelTickScope& operator=(NativeLevelTickScope const&) = delete;
};

void tickPlayback() {
    switch (playback::Playback::getInstance().getMode()) {
    case playback::PlaybackMode::Record:
        Recorder::getInstance().endTick(false);
        break;
    case playback::PlaybackMode::Replay:
        ReplaySession::getInstance().tick();
        break;
    case playback::PlaybackMode::Unknown:
    default:
        break;
    }
}

OfflineTickClaim claimOfflineTick() {
    std::scoped_lock lock(gOfflineTickGateMutex);
    if (!gOfflineTickGate.active) return {};

    OfflineTickClaim claim{true, std::nullopt};
    if (!gOfflineTickGate.pendingToken || gOfflineTickGate.inFlightToken) return claim;

    claim.token                    = gOfflineTickGate.pendingToken;
    gOfflineTickGate.inFlightToken = gOfflineTickGate.pendingToken;
    gOfflineTickGate.pendingToken.reset();

    auto const waited = static_cast<uint64_t>(std::max<int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - gOfflineTickGate.requestedAt
        )
            .count()
    ));
    auto&      pacing = gOfflineTickGate.pacing;
    ++pacing.claims;
    pacing.claimLatencyMicros    += waited;
    pacing.maxClaimLatencyMicros  = std::max(pacing.maxClaimLatencyMicros, waited);
    return claim;
}

void completeOfflineTick(uint64_t token, int replayTickBefore, int replayTickAfter) {
    std::scoped_lock lock(gOfflineTickGateMutex);
    if (!gOfflineTickGate.active || !gOfflineTickGate.inFlightToken || *gOfflineTickGate.inFlightToken != token) {
        return;
    }

    gOfflineTickGate.inFlightToken.reset();
    gOfflineTickGate.completedToken = token;
    gOfflineTickGate.completion     = OfflineReplayTickCompletion{
        token,
        replayTickBefore,
        replayTickAfter,
        true,
    };
    if (gOfflineRealTickHooked) gOfflineRealTicksOwed.fetch_add(1, std::memory_order_acq_rel);
}

bool offlineTickGateActive() {
    std::scoped_lock lock(gOfflineTickGateMutex);
    return gOfflineTickGate.active;
}

} // namespace

// The game ticks its renderers, its particles and the HUD timers from here: the held item's equip animation, the FOV
// and sky colour transitions, clouds, rain, block particles, item name and title timers. It does so once per tick of
// its real-time timer, which runs at 20 Hz however fast an export goes, so an export drops those calls and makes one
// for every replay tick it runs instead.
LL_TYPE_INSTANCE_HOOK(
    PlaybackClientRealTickHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$onTick,
    void,
    int nTick,
    int maxTick
) {
    if (!tOfflineRealTick) {
        gOfflineRealTickTarget.store(this, std::memory_order_release);
        if (offlineTickGateActive()) return;
    }
    origin(nTick, maxTick);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientUpdateHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$update,
    bool,
    bool isInitFinished
) {
    editor::tickReplayExportBeforeClientUpdate();
    exporting::grantPendingOfflineReplayTick();

    auto result = origin(isInitFinished);
    // The ticks stay owed until the game has made the call once.
    if (auto* const target = gOfflineRealTickTarget.load(std::memory_order_acquire)) {
        if (int const owed = gOfflineRealTicksOwed.exchange(0, std::memory_order_acq_rel); owed > 0) {
            tOfflineRealTick = true;
            for (int index = 0; index < owed; ++index) {
                // The held item's sway runs on a timer each of those ticks moves by the real time since the last.
                if (auto* const hand = getItemInHandRenderer()) {
                    hand->mLastTime = std::chrono::steady_clock::now() - std::chrono::milliseconds{50};
                }
                target->$onTick(index, owed);
            }
            tOfflineRealTick = false;
        }
    }

    if (isInitFinished) exporting::recoverEcoModeAfterInterruptedExport();
    editor::graphics::updateReplayMouseOwnership(*this);
    auto& replay = ReplaySession::getInstance();
    replay.updateControlPlane();
    // Gates the editor overlay, so loading, progress and sign-in screens must clear first.
    bool hudVisible = false;
    if (isInitFinished && replay.isActive() && replay.hasJoinedReplayWorld()) {
        auto const topScene = static_cast<unsigned int>(getTopSceneType());
        auto const hudScene = static_cast<unsigned int>(ui::SceneType::HudScene);
        hudVisible = (topScene & hudScene) != 0 && isInWorldAndNotShowingAnyMenuScreens() && !isShowingLoadingScreen()
                  && !isShowingProgressScreen() && !isShowingWorldProgressScreen();
    }
    editor::tickReplayUI(hudVisible);
    screen::tickRecordDialogs();
    replay.tryFinalizeWorldCleanup();
    advanceWindowClose();
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientLevelTickHook,
    ll::memory::HookPriority::High,
    ClientLevel,
    &ClientLevel::$_subTick,
    void
) {
    ChunkMutationBarrier::setActiveLevel(this);

    auto const offlineTick = claimOfflineTick();
    if (offlineTick.controlled) {
        if (!offlineTick.token) return;

        auto&     replay           = ReplaySession::getInstance();
        int const beforeReplayTick = replay.getAppliedReplayTick();
        tickPlayback();
        origin();
        [[maybe_unused]] auto tickBoundary    = ChunkMutationBarrier::enterTickBoundary(*this);
        int const             afterReplayTick = replay.getAppliedReplayTick();
        completeOfflineTick(*offlineTick.token, beforeReplayTick, afterReplayTick);
        return;
    }

    origin();
    [[maybe_unused]] auto tickBoundary = ChunkMutationBarrier::enterTickBoundary(*this);
    // Level::tick bumps the tick counter after _subTick; actors such as lingering clouds time their particles by it.
    NativeLevelTickScope nativeTick([this] {
        origin();
        // 26.40 does not export LevelData::incrementTick.
        ++getLevelData().mCurrentTick->tickID;
    });
    tickPlayback();
}

bool runNativeLevelTick() {
    if (!gNativeLevelTick) return false;
    gNativeLevelTick();
    return true;
}

bool hookClientTick(bool enable) {
    struct HookState {
        bool update{};
        bool levelTick{};
    };
    static HookState state;

    auto allInstalled  = [&] { return state.update && state.levelTick; };
    auto noneInstalled = [&] { return !state.update && !state.levelTick; };
    auto installAll    = [&] {
        if (!state.update) state.update = PlaybackClientUpdateHook::hook() == 0;
        if (!state.update) return false;
        if (!state.levelTick) state.levelTick = PlaybackClientLevelTickHook::hook() == 0;
        return state.levelTick;
    };
    auto removeAll = [&] {
        if (state.levelTick && PlaybackClientLevelTickHook::unhook()) state.levelTick = false;
        if (state.update && PlaybackClientUpdateHook::unhook()) state.update = false;
        return noneInstalled();
    };

    if (enable) {
        if (allInstalled()) {
            gClientTickHooksInstalled.store(true, std::memory_order_release);
            return true;
        }
        if (installAll()) {
            gClientTickHooksInstalled.store(true, std::memory_order_release);
            return true;
        }

        bool removed = removeAll();
        gClientTickHooksInstalled.store(false, std::memory_order_release);
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install client tick hooks (update={}, levelTick={}, rollback={})",
            state.update,
            state.levelTick,
            removed
        );
        return false;
    }

    endOfflineReplayTickGate();
    if (noneInstalled()) {
        gClientTickHooksInstalled.store(false, std::memory_order_release);
        return true;
    }
    if (removeAll()) {
        gClientTickHooksInstalled.store(false, std::memory_order_release);
        return true;
    }

    bool restored = installAll();
    gClientTickHooksInstalled.store(restored, std::memory_order_release);
    Playback::getInstance().getSelf().getLogger().error(
        "Unable to remove client tick hooks (update={}, levelTick={}, restoration={})",
        state.update,
        state.levelTick,
        restored
    );
    return false;
}

bool beginOfflineReplayTickGate() {
    if (!gClientTickHooksInstalled.load(std::memory_order_acquire)) return false;

    if (!gOfflineRealTickHooked) {
        gOfflineRealTickHooked = PlaybackClientRealTickHook::hook() == 0;
        if (!gOfflineRealTickHooked) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Unable to drive the client's real-time tick from the export; particles, held item and HUD timers "
                "follow the wall clock"
            );
        }
    }

    std::scoped_lock lock(gOfflineTickGateMutex);
    if (!gClientTickHooksInstalled.load(std::memory_order_relaxed) || gOfflineTickGate.active) return false;

    gOfflineRealTicksOwed.store(0, std::memory_order_release);
    gOfflineRealTickTarget.store(nullptr, std::memory_order_release);
    gOfflineTickGate.active = true;
    gOfflineTickGate.pendingToken.reset();
    gOfflineTickGate.inFlightToken.reset();
    gOfflineTickGate.completedToken.reset();
    gOfflineTickGate.completion.reset();
    return true;
}

void endOfflineReplayTickGate() {
    {
        std::scoped_lock lock(gOfflineTickGateMutex);
        gOfflineTickGate.active = false;
        gOfflineTickGate.pendingToken.reset();
        gOfflineTickGate.inFlightToken.reset();
        gOfflineTickGate.completedToken.reset();
        gOfflineTickGate.completion.reset();
    }
    gOfflineRealTicksOwed.store(0, std::memory_order_release);
    if (gOfflineRealTickHooked && PlaybackClientRealTickHook::unhook()) gOfflineRealTickHooked = false;
    gOfflineRealTickTarget.store(nullptr, std::memory_order_release);
}

OfflineReplayTickRequestResult requestOfflineReplayTick(OfflineReplayTickToken& token) {
    token = {};
    if (!gClientTickHooksInstalled.load(std::memory_order_acquire)) {
        return OfflineReplayTickRequestResult::Unavailable;
    }

    std::scoped_lock lock(gOfflineTickGateMutex);
    if (!gOfflineTickGate.active || !gClientTickHooksInstalled.load(std::memory_order_relaxed)) {
        return OfflineReplayTickRequestResult::Unavailable;
    }
    if (gOfflineTickGate.pendingToken || gOfflineTickGate.inFlightToken) {
        return OfflineReplayTickRequestResult::Busy;
    }

    token.id = gOfflineTickGate.nextTokenId++;
    if (gOfflineTickGate.nextTokenId == 0) ++gOfflineTickGate.nextTokenId;
    gOfflineTickGate.pendingToken = token.id;
    gOfflineTickGate.requestedAt  = std::chrono::steady_clock::now();
    ++gOfflineTickGate.pacing.requests;
    gOfflineTickGate.completedToken.reset();
    gOfflineTickGate.completion.reset();
    return OfflineReplayTickRequestResult::Requested;
}

bool wasOfflineReplayTickCompleted(OfflineReplayTickToken token) {
    if (!token) return false;

    std::scoped_lock lock(gOfflineTickGateMutex);
    return gOfflineTickGate.active && gOfflineTickGate.completedToken && *gOfflineTickGate.completedToken == token.id;
}

std::optional<OfflineReplayTickCompletion> getOfflineReplayTickCompletion(OfflineReplayTickToken token) {
    if (!token) return std::nullopt;

    std::scoped_lock lock(gOfflineTickGateMutex);
    if (!gOfflineTickGate.active || !gOfflineTickGate.completion || gOfflineTickGate.completion->token != token.id) {
        return std::nullopt;
    }
    return gOfflineTickGate.completion;
}

bool hasPendingOfflineReplayTick() {
    std::scoped_lock lock(gOfflineTickGateMutex);
    return gOfflineTickGate.active && gOfflineTickGate.pendingToken.has_value()
        && !gOfflineTickGate.inFlightToken.has_value();
}

OfflineReplayTickPacing takeOfflineReplayTickPacing() {
    std::scoped_lock lock(gOfflineTickGateMutex);
    return std::exchange(gOfflineTickGate.pacing, OfflineReplayTickPacing{});
}

} // namespace playback::runtime
