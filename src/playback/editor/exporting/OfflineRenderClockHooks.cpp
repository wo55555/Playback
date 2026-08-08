#include "OfflineRenderClockHooks.h"

#include "ExportActivity.h"
#include "playback/editor/keyframe/CameraTimelineRegistry.h"
#include "playback/functions/render/ReplayEntityInterpolator.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/MinecraftGame.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/ecs/strict/StrictEntityContext.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components/RenderPositionComponent.h"
#include "mc/entity/systems/UpdateRenderPosSystem.h"
#include "mc/platform/threading/Mutex.h"
#include "mc/util/Timer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>

namespace playback::editor::exporting {

namespace {

struct ActiveClockSample {
    OfflineRenderClockToken  token;
    OfflineRenderClockSample sample;
    bool                     claimed{};
    bool                     applied{};
};

struct AcquiredClockSample {
    OfflineRenderClockToken  token;
    OfflineRenderClockSample sample;
};

struct ActiveRenderSample {
    functions::render::ReplaySampleTime time;
    keyframe::CameraTimelineSource      source{keyframe::CameraTimelineSource::Preview};
};

std::atomic_bool                               gHookInstalled{false};
std::mutex                                     gClockMutex;
std::optional<ActiveClockSample>               gActiveSample;
uint64_t                                       gNextTokenId{1};
thread_local std::optional<ActiveRenderSample> gRenderSample;

class ScopedRenderSample {
public:
    ScopedRenderSample(functions::render::ReplaySampleTime time, keyframe::CameraTimelineSource source)
    : mPrevious(gRenderSample),
      mHadPrevious(gRenderSample.has_value()) {
        gRenderSample = ActiveRenderSample{time, source};
    }

    ~ScopedRenderSample() {
        if (mHadPrevious) gRenderSample = mPrevious;
        else gRenderSample.reset();
    }

    ScopedRenderSample(ScopedRenderSample const&)            = delete;
    ScopedRenderSample& operator=(ScopedRenderSample const&) = delete;

private:
    std::optional<ActiveRenderSample> mPrevious;
    bool                              mHadPrevious{};
};

class ScopedTimerOverride {
public:
    ScopedTimerOverride(Timer const& timer, OfflineRenderClockSample const& sample)
    : mTimer(const_cast<Timer&>(timer)),
      mTicksPerSecond(mTimer.mTicksPerSecond),
      mTicks(mTimer.mTicks),
      mAlpha(mTimer.mAlpha),
      mTimeScale(mTimer.mTimeScale),
      mPassedTime(mTimer.mPassedTime),
      mFrameStepAlignmentRemainder(mTimer.mFrameStepAlignmentRemainder),
      mLastTimeSeconds(mTimer.mLastTimeSeconds),
      mLastTimestep(mTimer.mLastTimestep),
      mOverflowTime(mTimer.mOverflowTime),
      mLastMs(mTimer.mLastMs),
      mLastMsSysTime(mTimer.mLastMsSysTime),
      mAdjustTime(mTimer.mAdjustTime),
      mSteppingTick(mTimer.mSteppingTick) {
        constexpr float ticksPerSecond = 20.0f;
        auto const      absoluteTick   = sample.replayTime.value();
        auto const      partialTick    = sample.replayTime.partialTick();
        auto const      absoluteMilliseconds =
            static_cast<int64>(std::llround(absoluteTick * 1000.0L / static_cast<long double>(ticksPerSecond)));

        mTimer.mTicksPerSecond              = ticksPerSecond;
        mTimer.mTicks                       = sample.wholeTicks;
        mTimer.mAlpha                       = partialTick;
        mTimer.mTimeScale                   = 1.0f;
        mTimer.mPassedTime                  = sample.deltaTicks;
        mTimer.mFrameStepAlignmentRemainder = 0.0f;
        mTimer.mLastTimeSeconds             = static_cast<float>(absoluteTick / ticksPerSecond);
        mTimer.mLastTimestep                = sample.deltaTicks / ticksPerSecond;
        mTimer.mOverflowTime                = 0.0f;
        mTimer.mLastMs                      = absoluteMilliseconds;
        mTimer.mLastMsSysTime               = absoluteMilliseconds;
        mTimer.mAdjustTime                  = 0.0f;
        mTimer.mSteppingTick                = partialTick;
    }

    ~ScopedTimerOverride() {
        mTimer.mTicksPerSecond              = mTicksPerSecond;
        mTimer.mTicks                       = mTicks;
        mTimer.mAlpha                       = mAlpha;
        mTimer.mTimeScale                   = mTimeScale;
        mTimer.mPassedTime                  = mPassedTime;
        mTimer.mFrameStepAlignmentRemainder = mFrameStepAlignmentRemainder;
        mTimer.mLastTimeSeconds             = mLastTimeSeconds;
        mTimer.mLastTimestep                = mLastTimestep;
        mTimer.mOverflowTime                = mOverflowTime;
        mTimer.mLastMs                      = mLastMs;
        mTimer.mLastMsSysTime               = mLastMsSysTime;
        mTimer.mAdjustTime                  = mAdjustTime;
        mTimer.mSteppingTick                = mSteppingTick;
    }

    ScopedTimerOverride(ScopedTimerOverride const&)            = delete;
    ScopedTimerOverride& operator=(ScopedTimerOverride const&) = delete;

private:
    Timer& mTimer;
    float  mTicksPerSecond;
    int    mTicks;
    float  mAlpha;
    float  mTimeScale;
    float  mPassedTime;
    float  mFrameStepAlignmentRemainder;
    float  mLastTimeSeconds;
    float  mLastTimestep;
    float  mOverflowTime;
    int64  mLastMs;
    int64  mLastMsSysTime;
    float  mAdjustTime;
    float  mSteppingTick;
};

std::optional<AcquiredClockSample> acquireClockSampleForRender() {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->claimed) return std::nullopt;
    // A single export sample owns exactly one Bedrock scene pass. This also
    // protects against a nested updateGraphics call consuming the same sample.
    gActiveSample->claimed = true;
    return AcquiredClockSample{gActiveSample->token, gActiveSample->sample};
}

void completeClockSample(OfflineRenderClockToken token, bool applied) {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != token.id) return;
    gActiveSample->applied = applied;
}

std::optional<ActiveRenderSample> currentRenderSample() noexcept { return gRenderSample; }

void applyCameraSample(mce::Camera& camera, keyframe::CameraTimelineSample const& sample) noexcept {
    auto const& state        = sample.state;
    float const yawRadians   = state.yaw * 3.14159265358979323846f / 180.0f;
    float const pitchRadians = state.pitch * 3.14159265358979323846f / 180.0f;
    float const cosPitch     = std::cos(pitchRadians);
    float const sinPitch     = std::sin(pitchRadians);
    float const sinYaw       = std::sin(yawRadians);
    float const cosYaw       = std::cos(yawRadians);

    ::glm::vec3 forward{-sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch};
    ::glm::vec3 right{cosYaw, 0.0f, sinYaw};
    ::glm::vec3 up{-sinYaw * sinPitch, cosPitch, cosYaw * sinPitch};
    float const rollRadians = state.roll * 3.14159265358979323846f / 180.0f;
    if (std::abs(rollRadians) > std::numeric_limits<float>::epsilon()) {
        float const       cosine      = std::cos(rollRadians);
        float const       sine        = std::sin(rollRadians);
        ::glm::vec3 const rolledRight = right * cosine + up * sine;
        ::glm::vec3 const rolledUp    = up * cosine - right * sine;
        right                         = rolledRight;
        up                            = rolledUp;
    }

    camera.mPosition = ::glm::vec3{state.x, state.y, state.z};
    camera.mForward  = forward;
    camera.mRight    = right;
    camera.mUp       = up;
    camera.mFov      = std::clamp(state.fov, 1.0f, 179.0f);
    if (sample.aspectRatio) camera.mAspectRatio = *sample.aspectRatio;
    camera.updateViewMatrixDependencies();
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
    if (sample) {
        bool poseApplied = false;
        {
            ScopedTimerOverride timerOverride(timer, sample->sample);
            ScopedRenderSample  renderSample(sample->sample.replayTime, keyframe::CameraTimelineSource::Export);
            auto                pose =
                functions::ReplaySession::getInstance().createReplayEntityRenderScope(sample->sample.replayTime);
            poseApplied = pose != nullptr;
            origin(client, timer);
        }
        completeClockSample(sample->token, poseApplied);
        return;
    }

    // ReplayExportDriver invokes updateGraphics explicitly. The host client
    // loop must not render the same scene a second time before Present.
    if (isOfflineRenderActivityActive()) return;

    auto& replay = functions::ReplaySession::getInstance();
    if (!replay.isActive() || !replay.hasJoinedReplayWorld()) {
        origin(client, timer);
        return;
    }

    auto const appliedTick = std::max(0, replay.getAppliedReplayTick());
    auto       previewTime = replay.isPaused() ? functions::render::ReplaySampleTime::fromRational(appliedTick, 1)
                                               : functions::render::ReplaySampleTime::fromPreview(
                                               std::max(0, appliedTick - 1),
                                               static_cast<float>(timer.mAlpha)
                                           );
    if (!previewTime) {
        origin(client, timer);
        return;
    }

    ScopedRenderSample renderSample(*previewTime, keyframe::CameraTimelineSource::Preview);
    origin(client, timer);
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraSetupHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::setupCamera,
    void,
    mce::Camera& camera,
    float        partialTick
) {
    if (auto const active = currentRenderSample()) {
        if (auto const sample = keyframe::sampleCameraTimeline(active->source, active->time)) {
            applyCameraSample(camera, *sample);
            origin(camera, partialTick);
            applyCameraSample(camera, *sample);
            return;
        }
    }
    origin(camera, partialTick);
}

LL_TYPE_STATIC_HOOK(
    ReplayEntityRenderPositionHook,
    ll::memory::HookPriority::Lowest,
    UpdateRenderPosSystem,
    &UpdateRenderPosSystem::_doUpdateRenderPosSystem,
    void,
    StrictEntityContext const&  context,
    StateVectorComponent const& stateVector,
    RenderPositionComponent&    renderPosition
) {
    origin(context, stateVector, renderPosition);
    functions::render::reapplyReplayEntityPosition(renderPosition);
}

} // namespace

bool hookOfflineRenderClock(bool enable) {
    struct HookState {
        bool clock{};
        bool camera{};
        bool entity{};
    };
    static HookState state;

    if (enable) {
        if (!state.clock) state.clock = OfflineRenderClockUpdateGraphicsHook::hook() == 0;
        if (!state.camera) state.camera = ReplayCameraSetupHook::hook() == 0;
        if (!state.entity) state.entity = ReplayEntityRenderPositionHook::hook() == 0;
        bool const ready = state.clock && state.camera && state.entity;
        gHookInstalled.store(ready, std::memory_order_release);
        return ready;
    }

    gHookInstalled.store(false, std::memory_order_release);
    resetOfflineRenderClock();
    if (state.entity && ReplayEntityRenderPositionHook::unhook()) state.entity = false;
    if (state.camera && ReplayCameraSetupHook::unhook()) state.camera = false;
    if (state.clock && OfflineRenderClockUpdateGraphicsHook::unhook()) state.clock = false;
    return !state.clock && !state.camera && !state.entity;
}

bool isOfflineRenderClockInstalled() { return gHookInstalled.load(std::memory_order_acquire); }

OfflineRenderClockPublishResult
publishOfflineRenderClockSample(OfflineRenderClockSample sample, OfflineRenderClockToken& token) {
    token = {};
    if (!sample.replayTime.isValid() || !std::isfinite(sample.deltaTicks) || sample.deltaTicks < 0.0f
        || sample.wholeTicks < 0) {
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
    return gActiveSample && gActiveSample->token.id == token.id && gActiveSample->applied;
}

void clearOfflineRenderClockSample(OfflineRenderClockToken token) {
    if (!token) return;
    std::scoped_lock lock(gClockMutex);
    if (gActiveSample && gActiveSample->token.id == token.id) gActiveSample.reset();
}

void resetOfflineRenderClock() {
    std::scoped_lock lock(gClockMutex);
    gActiveSample.reset();
    gRenderSample.reset();
}

} // namespace playback::editor::exporting
