#include "OfflineRenderClockHooks.h"

#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/platform/threading/Mutex.h"

#include "mc/client/game/MinecraftGame.h"
#include "mc/deps/ecs/strict/StrictEntityContext.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components/RenderPositionComponent.h"
#include "mc/entity/systems/UpdateRenderPosSystem.h"
#include "mc/util/Timer.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>

namespace playback::editor::exporting {

namespace {

struct ActiveClockSample {
    enum class RenderState : uint8_t { Pending, Applied, Failed };

    OfflineRenderClockToken  token;
    OfflineRenderClockSample sample;
    RenderState              renderState{RenderState::Pending};
};

struct AcquiredClockSample {
    OfflineRenderClockToken  token;
    OfflineRenderClockSample sample;
};

std::atomic_bool                 gHookInstalled{false};
std::mutex                       gClockMutex;
std::optional<ActiveClockSample> gActiveSample;
uint64_t                         gNextTokenId{1};

class ScopedTimerOverride {
public:
    ScopedTimerOverride(Timer const& timer, OfflineRenderClockSample const& sample)
    : mTimer(const_cast<Timer&>(timer)),
      mAlpha(mTimer.mAlpha),
      mPassedTime(mTimer.mPassedTime),
      mLastTimestep(mTimer.mLastTimestep) {
        // Bedrock exposes one render residual and one frame duration for the
        // fractional timing fields used during offline export.
        mTimer.mAlpha      = sample.partialTick;
        mTimer.mPassedTime = sample.partialTick;

        if (mTimer.mTicksPerSecond > 0.0f) {
            mTimer.mLastTimestep = sample.deltaTicks / mTimer.mTicksPerSecond;
        }
    }

    ~ScopedTimerOverride() {
        mTimer.mAlpha        = mAlpha;
        mTimer.mPassedTime   = mPassedTime;
        mTimer.mLastTimestep = mLastTimestep;
    }

    ScopedTimerOverride(ScopedTimerOverride const&)            = delete;
    ScopedTimerOverride& operator=(ScopedTimerOverride const&) = delete;

private:
    Timer& mTimer;
    float  mAlpha;
    float  mPassedTime;
    float  mLastTimestep;
};

class ScopedReplayRenderPose {
public:
    ScopedReplayRenderPose(functions::ReplaySession& replay, float partialTick)
    : mReplay(replay),
      mApplied(replay.beginExportRenderPose(partialTick)) {}

    ~ScopedReplayRenderPose() {
        if (mApplied) mReplay.endExportRenderPose();
    }

    ScopedReplayRenderPose(ScopedReplayRenderPose const&)            = delete;
    ScopedReplayRenderPose& operator=(ScopedReplayRenderPose const&) = delete;

    [[nodiscard]] bool applied() const { return mApplied; }

private:
    functions::ReplaySession& mReplay;
    bool                      mApplied;
};

std::optional<AcquiredClockSample> acquireClockSampleForRender() {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample) return std::nullopt;

    return AcquiredClockSample{gActiveSample->token, gActiveSample->sample};
}

void completeClockSample(OfflineRenderClockToken token, bool applied) {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != token.id) return;

    gActiveSample->renderState =
        applied ? ActiveClockSample::RenderState::Applied : ActiveClockSample::RenderState::Failed;
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderClockUpdateGraphicsHook,
    ll::memory::HookPriority::Lowest,
    MinecraftGame,
    &MinecraftGame::updateGraphics,
    void,
    Bedrock::NotNullNonOwnerPtr<IClientInstance> const& client,
    Timer const&                                        timer
) {
    auto const sample = acquireClockSampleForRender();
    if (!sample) {
        origin(client, timer);
        return;
    }

    ScopedTimerOverride    timerOverride(timer, sample->sample);
    auto&                  replay = functions::ReplaySession::getInstance();
    ScopedReplayRenderPose pose(replay, sample->sample.partialTick);
    origin(client, timer);
    completeClockSample(sample->token, pose.applied());
}

LL_TYPE_STATIC_HOOK(
    OfflineRenderPositionHook,
    ll::memory::HookPriority::Lowest,
    UpdateRenderPosSystem,
    &UpdateRenderPosSystem::_doUpdateRenderPosSystem,
    void,
    StrictEntityContext const&  context,
    StateVectorComponent const& stateVector,
    RenderPositionComponent&    renderPosition
) {
    origin(context, stateVector, renderPosition);
    (void)functions::ReplaySession::getInstance().applyExportRenderPosition(renderPosition);
}

} // namespace

bool hookOfflineRenderClock(bool enable) {
    struct HookState {
        bool graphics{};
        bool renderPosition{};
    };
    static HookState state;

    auto allInstalled  = [&] { return state.graphics && state.renderPosition; };
    auto noneInstalled = [&] { return !state.graphics && !state.renderPosition; };
    auto installAll    = [&] {
        if (!state.graphics) state.graphics = OfflineRenderClockUpdateGraphicsHook::hook() == 0;
        if (!state.graphics) return false;
        if (!state.renderPosition) state.renderPosition = OfflineRenderPositionHook::hook() == 0;
        return state.renderPosition;
    };
    auto removeAll = [&] {
        if (state.renderPosition && OfflineRenderPositionHook::unhook()) state.renderPosition = false;
        if (state.graphics && OfflineRenderClockUpdateGraphicsHook::unhook()) state.graphics = false;
        return noneInstalled();
    };

    if (enable) {
        if (allInstalled()) {
            gHookInstalled.store(true, std::memory_order_release);
            return true;
        }
        if (!installAll()) {
            (void)removeAll();
            gHookInstalled.store(false, std::memory_order_release);
            return false;
        }

        gHookInstalled.store(true, std::memory_order_release);
        return true;
    }

    gHookInstalled.store(false, std::memory_order_release);
    resetOfflineRenderClock();
    if (noneInstalled()) return true;
    if (removeAll()) return true;

    bool const restored = installAll();
    gHookInstalled.store(restored, std::memory_order_release);
    if (!restored) {
        return removeAll();
    }
    return false;
}

bool isOfflineRenderClockInstalled() { return gHookInstalled.load(std::memory_order_acquire); }

OfflineRenderClockPublishResult
publishOfflineRenderClockSample(OfflineRenderClockSample sample, OfflineRenderClockToken& token) {
    token = {};
    if (!std::isfinite(sample.partialTick) || !std::isfinite(sample.deltaTicks) || sample.partialTick < 0.0f
        || sample.partialTick >= 1.0f || sample.deltaTicks < 0.0f) {
        return OfflineRenderClockPublishResult::InvalidSample;
    }
    if (!gHookInstalled.load(std::memory_order_acquire)) return OfflineRenderClockPublishResult::Unavailable;

    std::scoped_lock lock(gClockMutex);
    if (!gHookInstalled.load(std::memory_order_relaxed)) return OfflineRenderClockPublishResult::Unavailable;
    if (gActiveSample) return OfflineRenderClockPublishResult::Busy;

    token.id = gNextTokenId++;
    if (gNextTokenId == 0) ++gNextTokenId;
    gActiveSample = ActiveClockSample{token, sample};
    return OfflineRenderClockPublishResult::Published;
}

bool wasOfflineRenderClockSampleApplied(OfflineRenderClockToken token) {
    if (!token) return false;

    std::scoped_lock lock(gClockMutex);
    return gActiveSample && gActiveSample->token.id == token.id
        && gActiveSample->renderState == ActiveClockSample::RenderState::Applied;
}

bool didOfflineRenderClockSampleFail(OfflineRenderClockToken token) {
    if (!token) return false;

    std::scoped_lock lock(gClockMutex);
    return gActiveSample && gActiveSample->token.id == token.id
        && gActiveSample->renderState == ActiveClockSample::RenderState::Failed;
}

void clearOfflineRenderClockSample(OfflineRenderClockToken token) {
    if (!token) return;

    std::scoped_lock lock(gClockMutex);
    if (gActiveSample && gActiveSample->token.id == token.id) gActiveSample.reset();
}

void resetOfflineRenderClock() {
    std::scoped_lock lock(gClockMutex);
    gActiveSample.reset();
}

} // namespace playback::editor::exporting
