#include "ClientTickHooks.h"

#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/editor/graphics/ReplayMouseHook.h"
#include "playback/exporting/ExportEcoMode.h"
#include "playback/record/ChunkMutationBarrier.h"
#include "playback/record/Recorder.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/SceneType.h"
#include "mc/client/multiplayer/MultiPlayerLevel.h"
#include "mc/world/level/storage/LevelData.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientUpdateHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$update,
    bool,
    bool isInitFinished
) {
    editor::tickReplayExportBeforeClientUpdate();
    auto result = origin(isInitFinished);
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
    replay.tryFinalizeWorldCleanup();
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientLevelTickHook,
    ll::memory::HookPriority::High,
    MultiPlayerLevel,
    &MultiPlayerLevel::$_subTick,
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
        getLevelData().incrementTick();
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

    std::scoped_lock lock(gOfflineTickGateMutex);
    if (!gClientTickHooksInstalled.load(std::memory_order_relaxed) || gOfflineTickGate.active) return false;

    gOfflineTickGate.active = true;
    gOfflineTickGate.pendingToken.reset();
    gOfflineTickGate.inFlightToken.reset();
    gOfflineTickGate.completedToken.reset();
    gOfflineTickGate.completion.reset();
    return true;
}

void endOfflineReplayTickGate() {
    std::scoped_lock lock(gOfflineTickGateMutex);
    gOfflineTickGate.active = false;
    gOfflineTickGate.pendingToken.reset();
    gOfflineTickGate.inFlightToken.reset();
    gOfflineTickGate.completedToken.reset();
    gOfflineTickGate.completion.reset();
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
