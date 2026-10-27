#include "ReplayEntityInterpolator.h"

#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/entity/components/MovementInterpolatorComponent.h"
#include "mc/entity/components/RenderPositionComponent.h"
#include "mc/entity/components/RenderRotationComponent.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffect.h"
#include "mc/world/effect/MobEffectInstance.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace playback::visuals {

namespace {

struct EntityRenderKeyHash {
    size_t operator()(EntityRenderKey const& key) const noexcept {
        auto const combined = (static_cast<uint64_t>(key.registryId) << 32U) | key.entityId;
        return std::hash<uint64_t>{}(combined);
    }
};

struct EntityPoseHistory {
    int64_t          previousTick{};
    int64_t          currentTick{};
    EntityRenderPose previous{};
    EntityRenderPose current{};
};

struct PendingEntityPose {
    EntityRenderPose previous{};
    EntityRenderPose current{};
};

// Camera position minus entity position of the replay subject; it changes while the subject stands still.
struct CameraOffsetSample {
    EntityRenderPosition            offset{};
    float                           fov{}; // degrees, 0 when the renderer had produced none yet
    std::optional<EntityRenderPose> view{}; // absolute camera pose that replaces entity pose + offset
    float                           viewWeight{}; // 0 = entity pose + offset, 1 = view
    float                           sleepTicks{}; // consecutive ticks spent in the view, 0 outside it
};

struct CameraOffsetHistory {
    int64_t            previousTick{};
    int64_t            currentTick{};
    CameraOffsetSample previous{};
    CameraOffsetSample current{};
};

struct PendingCameraOffset {
    CameraOffsetSample previous{};
    CameraOffsetSample current{};
};

using EntityPoseMap = std::unordered_map<EntityRenderKey, EntityPoseHistory, EntityRenderKeyHash>;

std::atomic<std::shared_ptr<EntityPoseMap const>>                           gEntityPoses;
std::atomic<std::shared_ptr<CameraOffsetHistory const>>                     gCameraOffset;
std::mutex                                                                  gPendingMutex;
std::unordered_map<EntityRenderKey, PendingEntityPose, EntityRenderKeyHash> gPending;
std::optional<PendingCameraOffset>                                          gPendingCameraOffset;

constexpr uint64_t   kNoSubject = std::numeric_limits<uint64_t>::max();
std::atomic_uint64_t gSubject{kNoSubject};
std::atomic_bool     gSubjectIsRecorder{true};
std::atomic_uint8_t  gSubjectHidden{0};
std::atomic<float>   gSubjectViewWeight{0.0f}; // blend of the recorded sleeping view at the last followed sample
std::atomic<float>   gSubjectSleepTicks{0.0f}; // consecutive sleeping ticks at the last followed sample

std::optional<EntityRenderKey> subjectKey() noexcept {
    auto const packed = gSubject.load(std::memory_order_acquire);
    if (packed == kNoSubject) return std::nullopt;
    return EntityRenderKey{static_cast<uint32_t>(packed >> 32U), static_cast<uint32_t>(packed & 0xFFFFFFFFU)};
}

constexpr int64_t kNauseaFreshNs = 500'000'000;

std::atomic_int64_t gNauseaStamp{0};
std::atomic_bool    gNauseaPresent{false};
std::atomic_bool    gNauseaActive{false};
std::atomic_int     gNauseaDuration{0};
std::atomic<double> gNauseaTick{0.0};

int64_t steadyNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void publishSubjectNausea(Actor const& subject, long double tick) {
    auto*       confusion = MobEffect::CONFUSION();
    auto const* effect    = confusion ? subject.getEffect(*confusion) : nullptr;
    int const   duration  = effect ? effect->getDuration().mValue : 0;
    gNauseaPresent.store(effect != nullptr, std::memory_order_relaxed);
    gNauseaDuration.store(duration, std::memory_order_relaxed);
    gNauseaActive.store(effect && (duration == -1 || duration > 60), std::memory_order_relaxed);
    gNauseaTick.store(static_cast<double>(tick), std::memory_order_relaxed);
    gNauseaStamp.store(steadyNs(), std::memory_order_release);
}

bool finite(EntityRenderPosition const& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(EntityRenderPose const& value) noexcept {
    return finite(value.position) && std::isfinite(value.pitch) && std::isfinite(value.yaw)
        && std::isfinite(value.headYaw) && std::isfinite(value.bodyYaw);
}

float interpolateAngle(float previous, float current, float amount) noexcept {
    return previous + std::remainder(current - previous, 360.0f) * amount;
}

EntityRenderPose samplePose(EntityPoseHistory const& history, ReplaySampleTime const& sample) noexcept {
    auto const time = sample.value();
    if (time <= static_cast<long double>(history.previousTick)) return history.previous;
    if (time >= static_cast<long double>(history.currentTick) || history.currentTick <= history.previousTick) {
        return history.current;
    }

    float const amount = static_cast<float>(
        (time - static_cast<long double>(history.previousTick))
        / static_cast<long double>(history.currentTick - history.previousTick)
    );
    return EntityRenderPose{
        {
         std::lerp(history.previous.position.x, history.current.position.x, amount),
         std::lerp(history.previous.position.y, history.current.position.y, amount),
         std::lerp(history.previous.position.z, history.current.position.z, amount),
         },
        interpolateAngle(history.previous.pitch, history.current.pitch, amount),
        interpolateAngle(history.previous.yaw, history.current.yaw, amount),
        interpolateAngle(history.previous.headYaw, history.current.headYaw, amount),
        interpolateAngle(history.previous.bodyYaw, history.current.bodyYaw, amount),
    };
}

CameraOffsetSample sampleCameraOffset(CameraOffsetHistory const& history, ReplaySampleTime const& sample) noexcept {
    auto const time = sample.value();
    if (time <= static_cast<long double>(history.previousTick)) return history.previous;
    if (time >= static_cast<long double>(history.currentTick) || history.currentTick <= history.previousTick) {
        return history.current;
    }

    float const amount = static_cast<float>(
        (time - static_cast<long double>(history.previousTick))
        / static_cast<long double>(history.currentTick - history.previousTick)
    );
    auto const& previous = history.previous;
    auto const& current  = history.current;
    // A zero FOV means the renderer had produced none yet and must not be blended in.
    float const fov =
        previous.fov > 0.0f && current.fov > 0.0f ? std::lerp(previous.fov, current.fov, amount) : current.fov;
    std::optional<EntityRenderPose> view = current.view ? current.view : previous.view;
    // Entering or leaving the view fades its weight instead of switching on a tick boundary.
    float viewWeight = previous.view && current.view ? 1.0f : current.view ? amount : previous.view ? 1.0f - amount : 0.0f;
    if (previous.view && current.view) {
        auto const& a = *previous.view;
        auto const& b = *current.view;
        view          = EntityRenderPose{
            {
             std::lerp(a.position.x, b.position.x, amount),
             std::lerp(a.position.y, b.position.y, amount),
             std::lerp(a.position.z, b.position.z, amount),
             },
            interpolateAngle(a.pitch, b.pitch, amount),
            interpolateAngle(a.yaw, b.yaw, amount),
            interpolateAngle(a.headYaw, b.headYaw, amount),
            interpolateAngle(a.bodyYaw, b.bodyYaw, amount),
        };
    }
    return CameraOffsetSample{
        {
         std::lerp(previous.offset.x, current.offset.x, amount),
         std::lerp(previous.offset.y, current.offset.y, amount),
         std::lerp(previous.offset.z, current.offset.z, amount),
         },
        fov,
        view,
        viewWeight,
        std::lerp(previous.sleepTicks, current.sleepTicks, amount),
    };
}

std::optional<CameraOffsetSample> sampleSubjectCamera(ReplaySampleTime const& sample) {
    if (auto const history = gCameraOffset.load(std::memory_order_acquire)) return sampleCameraOffset(*history, sample);
    // The snapshot's sample stays pending until the first tick commits it.
    std::scoped_lock lock(gPendingMutex);
    if (gPendingCameraOffset) return gPendingCameraOffset->current;
    return std::nullopt;
}

// Local-player camera offset measured 2026-10-25: standing 0, sneak 0.35 below, swim 1.22 below, ~8 ticks.
constexpr float kSneakCameraOffsetY = -0.35f;
constexpr float kSwimCameraOffsetY  = -1.22f;
constexpr float kPostureBlend       = 0.3f;

std::atomic<float> gPostureOffsetY{0.0f};
std::atomic<float> gPostureViewWeight{0.0f};
std::atomic<float> gPostureSleepTicks{0.0f};
int64_t            gPostureTick{-1};

void resetPosture() noexcept {
    gPostureOffsetY.store(0.0f, std::memory_order_relaxed);
    gPostureViewWeight.store(0.0f, std::memory_order_relaxed);
    gPostureSleepTicks.store(0.0f, std::memory_order_relaxed);
    gPostureTick = -1;
}

} // namespace

struct ScopedReplayEntityPose::State {
    struct ActorState {
        StateVectorComponent*          stateVector{};
        Vec3                           statePosition{};
        Vec3                           statePositionPrevious{};
        Vec3                           statePositionDelta{};
        RenderPositionComponent*       renderPosition{};
        Vec3                           renderPositionValue{};
        Vec3                           exportPosition{};
        MovementInterpolatorComponent* movementInterpolator{};
        Vec3                           movementPosition{};
        Vec2                           movementRotation{};
        float                          movementHeadYaw{};
        int                            movementPositionSteps{};
        int                            movementRotationSteps{};
        int                            movementHeadYawSteps{};
        RenderRotationComponent*       renderRotation{};
        Vec2                           renderRotationValue{};
        ActorRotationComponent*        actorRotation{};
        Vec2                           actorRotationValue{};
        Vec2                           actorRotationPreviousValue{};
        ActorHeadRotationComponent*    headRotation{};
        float                          headYaw{};
        float                          previousHeadYaw{};
        MobBodyRotationComponent*      bodyRotation{};
        float                          bodyYaw{};
        float                          previousBodyYaw{};
    };

    struct HiddenActor {
        Actor* actor{};
        bool   invisible{};
        bool   hiddenWhenInvisible{};
    };

    std::vector<ActorState>    actors;
    std::optional<HiddenActor> hidden;
    State*                     previous{};

    ~State() {
        if (gActiveState == this) gActiveState = previous;
        if (hidden) {
            hidden->actor->setStatusFlag(ActorFlags::HiddenWhenInvisible, hidden->hiddenWhenInvisible);
            hidden->actor->setStatusFlag(ActorFlags::Invisible, hidden->invisible);
        }
        for (auto state = actors.rbegin(); state != actors.rend(); ++state) {
            state->stateVector->mPos      = state->statePosition;
            state->stateVector->mPosPrev  = state->statePositionPrevious;
            state->stateVector->mPosDelta = state->statePositionDelta;
            state->renderPosition->mValue = state->renderPositionValue;
            if (state->movementInterpolator) {
                *state->movementInterpolator->mPos          = state->movementPosition;
                *state->movementInterpolator->mRot          = state->movementRotation;
                state->movementInterpolator->mHeadYaw       = state->movementHeadYaw;
                state->movementInterpolator->mPositionSteps = state->movementPositionSteps;
                state->movementInterpolator->mRotationSteps = state->movementRotationSteps;
                state->movementInterpolator->mHeadYawSteps  = state->movementHeadYawSteps;
            }
            if (state->renderRotation) state->renderRotation->mRot = state->renderRotationValue;
            state->actorRotation->mRot     = state->actorRotationValue;
            state->actorRotation->mRotPrev = state->actorRotationPreviousValue;
            if (state->headRotation) {
                state->headRotation->mYHeadRot  = state->headYaw;
                state->headRotation->mYHeadRotO = state->previousHeadYaw;
            }
            if (state->bodyRotation) {
                state->bodyRotation->mYBodyRot  = state->bodyYaw;
                state->bodyRotation->mYBodyRotO = state->previousBodyYaw;
            }
        }
    }

    static thread_local State* gActiveState;
};

thread_local ScopedReplayEntityPose::State* ScopedReplayEntityPose::State::gActiveState = nullptr;

ScopedReplayEntityPose::ScopedReplayEntityPose(std::unique_ptr<State> state) noexcept : mState(std::move(state)) {}

ScopedReplayEntityPose::~ScopedReplayEntityPose() = default;

bool isReplayEntityHiddenInScope(Actor const& actor) {
    auto const* state = ScopedReplayEntityPose::State::gActiveState;
    return state && state->hidden && state->hidden->actor == &actor;
}

Actor* replayHiddenActorInScope() {
    auto const* state = ScopedReplayEntityPose::State::gActiveState;
    return state && state->hidden ? state->hidden->actor : nullptr;
}

std::optional<SubjectSleepSignals> replaySubjectSleepInScope() {
    auto* actor = replayHiddenActorInScope();
    if (!actor) return std::nullopt;
    return SubjectSleepSignals{
        actor->isSleeping(),
        actor->getStatusFlag(ActorFlags::Sleeping),
        gSubjectViewWeight.load(std::memory_order_relaxed),
    };
}

float replaySubjectViewWeight() { return gSubjectViewWeight.load(std::memory_order_relaxed); }

float replaySubjectSleepTicks() { return gSubjectSleepTicks.load(std::memory_order_relaxed); }

std::optional<SubjectNauseaSample> sampleReplaySubjectNausea() {
    auto const stamp = gNauseaStamp.load(std::memory_order_acquire);
    // An export pass can take longer than the freshness window, and it only ends by clearing the hide reason.
    bool const exporting =
        (gSubjectHidden.load(std::memory_order_acquire) & static_cast<uint8_t>(SubjectHideReason::Export)) != 0;
    if (stamp == 0 || (!exporting && steadyNs() - stamp > kNauseaFreshNs)) return std::nullopt;
    return SubjectNauseaSample{
        gNauseaPresent.load(std::memory_order_relaxed),
        gNauseaDuration.load(std::memory_order_relaxed),
        gNauseaActive.load(std::memory_order_relaxed),
        static_cast<long double>(gNauseaTick.load(std::memory_order_relaxed)),
    };
}

std::unique_ptr<ScopedReplayEntityPose>
createReplayEntityRenderScope(std::vector<EntityRenderTarget> const& targets, ReplaySampleTime const& sample) {
    if (!sample.isValid() || ScopedReplayEntityPose::State::gActiveState) return {};

    try {
        auto const poses = gEntityPoses.load(std::memory_order_acquire);
        auto       state = std::make_unique<ScopedReplayEntityPose::State>();
        state->actors.reserve(targets.size());

        // Frames right after a dimension switch have no committed poses yet; the hidden subject must still be hidden.
        auto const subject = gSubjectHidden.load(std::memory_order_acquire) != 0 ? subjectKey() : std::nullopt;
        for (auto const& target : targets) {
            // A despawned or unloaded entity must not cost the rest of the frame its interpolation.
            if (!target.actor) continue;

            if (subject && target.key == *subject && !state->hidden) {
                state->hidden = ScopedReplayEntityPose::State::HiddenActor{
                    target.actor,
                    target.actor->getStatusFlag(ActorFlags::Invisible),
                    target.actor->getStatusFlag(ActorFlags::HiddenWhenInvisible),
                };
                target.actor->setStatusFlag(ActorFlags::Invisible, true);
                target.actor->setStatusFlag(ActorFlags::HiddenWhenInvisible, true);
                publishSubjectNausea(*target.actor, sample.value());
            }

            if (!poses) continue;
            auto const history = poses->find(target.key);
            if (history == poses->end()) continue;

            auto& context              = target.actor->getEntityContext();
            auto* renderPosition       = context.tryGetComponent<RenderPositionComponent>().as_ptr();
            auto* renderRotation       = context.tryGetComponent<RenderRotationComponent>().as_ptr();
            auto* movementInterpolator = context.tryGetComponent<MovementInterpolatorComponent>().as_ptr();
            auto* stateVector          = target.actor->mBuiltInComponents->mStateVectorComponent.get();
            auto* actorRotation        = target.actor->mBuiltInComponents->mActorRotationComponent.get();
            auto* headRotation         = context.tryGetComponent<ActorHeadRotationComponent>().as_ptr();
            auto* bodyRotation         = context.tryGetComponent<MobBodyRotationComponent>().as_ptr();
            if (!stateVector || !renderPosition || !actorRotation) continue;

            // A passenger is positioned by its vehicle, so only its rotation may be interpolated.
            bool const positioned = !target.actor->isRiding();
            auto const sampled    = samplePose(history->second, sample);
            state->actors.emplace_back(
                ScopedReplayEntityPose::State::ActorState{
                    stateVector,
                    stateVector->mPos.get(),
                    stateVector->mPosPrev.get(),
                    stateVector->mPosDelta.get(),
                    renderPosition,
                    renderPosition->mValue.get(),
                    Vec3{sampled.position.x, sampled.position.y, sampled.position.z},
                    movementInterpolator,
                    movementInterpolator ? movementInterpolator->mPos.get() : Vec3{},
                    movementInterpolator ? movementInterpolator->mRot.get() : Vec2{},
                    movementInterpolator ? movementInterpolator->mHeadYaw : 0.0f,
                    movementInterpolator ? movementInterpolator->mPositionSteps : 0,
                    movementInterpolator ? movementInterpolator->mRotationSteps : 0,
                    movementInterpolator ? movementInterpolator->mHeadYawSteps : 0,
                    renderRotation,
                    renderRotation ? renderRotation->mRot.get() : Vec2{},
                    actorRotation,
                    actorRotation->mRot.get(),
                    actorRotation->mRotPrev.get(),
                    headRotation,
                    headRotation ? static_cast<float>(headRotation->mYHeadRot) : 0.0f,
                    headRotation ? static_cast<float>(headRotation->mYHeadRotO) : 0.0f,
                    bodyRotation,
                    bodyRotation ? static_cast<float>(bodyRotation->mYBodyRot) : 0.0f,
                    bodyRotation ? static_cast<float>(bodyRotation->mYBodyRotO) : 0.0f,
            }
            );

            auto& applied = state->actors.back();
            if (positioned) {
                applied.stateVector->mPos     = applied.exportPosition;
                applied.stateVector->mPosPrev = applied.stateVector->mPos;
                // The hidden subject keeps its per-tick movement: its first-person hand bob is driven by it.
                bool const isSubject           = state->hidden && state->hidden->actor == target.actor;
                applied.stateVector->mPosDelta = isSubject ? applied.statePositionDelta : Vec3{};
                applied.renderPosition->mValue = applied.exportPosition;
            }
            if (applied.movementInterpolator) {
                if (positioned) *applied.movementInterpolator->mPos = applied.exportPosition;
                *applied.movementInterpolator->mRot          = Vec2{sampled.pitch, sampled.yaw};
                applied.movementInterpolator->mHeadYaw       = sampled.headYaw;
                applied.movementInterpolator->mPositionSteps = 0;
                applied.movementInterpolator->mRotationSteps = 0;
                applied.movementInterpolator->mHeadYawSteps  = 0;
            }
            if (applied.renderRotation) {
                applied.renderRotation->mRot = Vec2{sampled.pitch, sampled.yaw};
            }
            applied.actorRotation->mRot     = Vec2{sampled.pitch, sampled.yaw};
            applied.actorRotation->mRotPrev = Vec2{sampled.pitch, sampled.yaw};
            if (applied.headRotation) {
                applied.headRotation->mYHeadRot  = sampled.headYaw;
                applied.headRotation->mYHeadRotO = sampled.headYaw;
            }
            if (applied.bodyRotation) {
                applied.bodyRotation->mYBodyRot  = sampled.bodyYaw;
                applied.bodyRotation->mYBodyRotO = sampled.bodyYaw;
            }
        }

        if (!state->hidden) gNauseaStamp.store(0, std::memory_order_release);
        if (!poses && !state->hidden) return {};

        state->previous                             = ScopedReplayEntityPose::State::gActiveState;
        ScopedReplayEntityPose::State::gActiveState = state.get();
        return std::unique_ptr<ScopedReplayEntityPose>(new ScopedReplayEntityPose(std::move(state)));
    } catch (...) {
        return {};
    }
}

void queueReplayEntityPose(EntityRenderKey key, EntityRenderPose previous, EntityRenderPose current) {
    if (!finite(previous) || !finite(current)) return;
    std::scoped_lock lock(gPendingMutex);
    auto [entry, inserted] = gPending.try_emplace(key, PendingEntityPose{previous, current});
    if (!inserted) entry->second.current = current;
}

void queueReplaySubjectCameraOffset(EntityRenderPosition offset, float fov, std::optional<EntityRenderPose> view) {
    if (!finite(offset)) return;
    if (view && !finite(*view)) view.reset();
    CameraOffsetSample const sample{
        offset,
        std::isfinite(fov) && fov > 1.0f && fov < 179.0f ? fov : 0.0f,
        view,
        view ? 1.0f : 0.0f
    };
    std::scoped_lock         lock(gPendingMutex);
    if (gPendingCameraOffset) gPendingCameraOffset->current = sample;
    else gPendingCameraOffset = PendingCameraOffset{sample, sample};
}

void commitReplayEntityPoses(int64_t tick) {
    if (tick < 0) return;
    std::scoped_lock lock(gPendingMutex);
    if (gPendingCameraOffset) {
        auto const existing = gCameraOffset.load(std::memory_order_acquire);
        auto const previous = existing ? existing->current : gPendingCameraOffset->previous;
        gPendingCameraOffset->current.sleepTicks =
            gPendingCameraOffset->current.view ? previous.sleepTicks + 1.0f : 0.0f;
        gCameraOffset.store(
            std::make_shared<CameraOffsetHistory const>(
                CameraOffsetHistory{std::max<int64_t>(0, tick - 1), tick, previous, gPendingCameraOffset->current}
            ),
            std::memory_order_release
        );
        gPendingCameraOffset.reset();
    }
    if (gPending.empty()) return;

    auto const current = gEntityPoses.load(std::memory_order_acquire);
    auto       next    = current ? std::make_shared<EntityPoseMap>(*current) : std::make_shared<EntityPoseMap>();
    next->reserve(next->size() + gPending.size());
    for (auto const& [key, pending] : gPending) {
        auto const existing = next->find(key);
        auto const previous = existing == next->end() ? pending.previous : existing->second.current;
        next->insert_or_assign(key, EntityPoseHistory{std::max<int64_t>(0, tick - 1), tick, previous, pending.current});
    }
    gPending.clear();
    gEntityPoses.store(std::move(next), std::memory_order_release);
}

void removeReplayEntityPose(EntityRenderKey key) {
    std::scoped_lock lock(gPendingMutex);
    gPending.erase(key);
    auto const current = gEntityPoses.load(std::memory_order_acquire);
    if (!current || !current->contains(key)) return;
    auto next = std::make_shared<EntityPoseMap>(*current);
    next->erase(key);
    gEntityPoses.store(std::move(next), std::memory_order_release);
}

void clearReplayEntityPoses() {
    std::scoped_lock lock(gPendingMutex);
    gPending.clear();
    gPendingCameraOffset.reset();
    gCameraOffset.store({}, std::memory_order_release);
    gEntityPoses.store({}, std::memory_order_release);
}

void setReplaySubject(std::optional<EntityRenderKey> key) {
    if (!key) {
        gSubjectIsRecorder.store(true, std::memory_order_release);
        resetPosture();
    }
    gSubject.store(
        key ? (static_cast<uint64_t>(key->registryId) << 32U) | key->entityId : kNoSubject,
        std::memory_order_release
    );
}

void noteReplaySubjectPosture(int64_t tick, bool snap, ReplaySubjectPosture const& posture) {
    bool const  recorder = isReplaySubjectRecorder();
    float const target   = !recorder && posture.swimming ? kSwimCameraOffsetY
                         : !recorder && posture.sneaking ? kSneakCameraOffsetY
                                                         : 0.0f;
    bool const  sleeping = posture.sleeping || posture.actorSleeping;
    float       offset   = gPostureOffsetY.load(std::memory_order_relaxed);
    float       slept    = gPostureSleepTicks.load(std::memory_order_relaxed);
    if (snap || gPostureTick < 0) {
        offset = target;
        slept  = sleeping && !recorder ? 1.0f : 0.0f;
    } else if (tick == gPostureTick) {
        return;
    } else {
        offset += (target - offset) * kPostureBlend;
        if (std::abs(target - offset) < 0.001f) offset = target;
        slept = sleeping && !recorder ? slept + 1.0f : 0.0f;
    }
    gPostureTick = tick;
    float const weight = sleeping && !recorder ? 1.0f : 0.0f;
    gPostureOffsetY.store(offset, std::memory_order_relaxed);
    gPostureViewWeight.store(weight, std::memory_order_relaxed);
    gPostureSleepTicks.store(slept, std::memory_order_relaxed);
    if (!recorder) {
        gSubjectViewWeight.store(weight, std::memory_order_relaxed);
        gSubjectSleepTicks.store(slept, std::memory_order_relaxed);
    }
}

float replaySubjectPostureOffsetY() noexcept { return gPostureOffsetY.load(std::memory_order_relaxed); }

float replaySubjectPostureViewWeight() noexcept { return gPostureViewWeight.load(std::memory_order_relaxed); }

float replaySubjectPostureSleepTicks() noexcept { return gPostureSleepTicks.load(std::memory_order_relaxed); }

void setReplaySubjectRecorder(bool recorder) noexcept {
    gSubjectIsRecorder.store(recorder, std::memory_order_release);
}

bool isReplaySubjectRecorder() noexcept { return gSubjectIsRecorder.load(std::memory_order_acquire); }

bool hasReplaySubject() noexcept { return gSubject.load(std::memory_order_acquire) != kNoSubject; }

bool isReplaySubject(EntityRenderKey const& key) noexcept {
    uint64_t const packed = (static_cast<uint64_t>(key.registryId) << 32U) | key.entityId;
    return gSubject.load(std::memory_order_acquire) == packed;
}

void setReplaySubjectHidden(SubjectHideReason reason, bool hidden) {
    auto const bit = static_cast<uint8_t>(reason);
    if (hidden) gSubjectHidden.fetch_or(bit, std::memory_order_acq_rel);
    else gSubjectHidden.fetch_and(static_cast<uint8_t>(~bit), std::memory_order_acq_rel);
    if (reason != SubjectHideReason::Session && !hidden) {
        gSubjectViewWeight.store(0.0f, std::memory_order_relaxed);
        gSubjectSleepTicks.store(0.0f, std::memory_order_relaxed);
    }
}

bool isReplaySubjectFollowed() {
    return (gSubjectHidden.load(std::memory_order_acquire) & static_cast<uint8_t>(SubjectHideReason::Follow)) != 0;
}

bool isReplaySubjectViewDriven() {
    constexpr auto mask = static_cast<uint8_t>(SubjectHideReason::Follow) | static_cast<uint8_t>(SubjectHideReason::Export);
    return (gSubjectHidden.load(std::memory_order_acquire) & mask) != 0;
}

std::optional<EntityRenderPose> sampleReplaySubjectPose(ReplaySampleTime const& sample) {
    auto const subject = subjectKey();
    if (!subject) return std::nullopt;
    EntityRenderKey const key = *subject;

    std::optional<EntityRenderPose> found;
    if (auto const poses = gEntityPoses.load(std::memory_order_acquire)) {
        if (auto const entry = poses->find(key); entry != poses->end()) found = samplePose(entry->second, sample);
    }
    if (!found) {
        // The seeded pose stays pending until the first tick commits it.
        std::scoped_lock lock(gPendingMutex);
        if (auto const pending = gPending.find(key); pending != gPending.end()) found = pending->second.current;
    }
    if (!found) return std::nullopt;
    auto pose = *found;
    // Recorded positions are already standing eye height. The recorder uses its recorded camera offset.
    // Another player has no such track, so sneak and swim add the offset measured on the local player.
    if (!isReplaySubjectRecorder()) {
        pose.position.y += replaySubjectPostureOffsetY();
        return pose;
    }
    if (auto const camera = sampleSubjectCamera(sample)) {
        pose.position.x += camera->offset.x;
        pose.position.y += camera->offset.y;
        pose.position.z += camera->offset.z;
        if (camera->view && camera->viewWeight > 0.0f) {
            // The recorded game camera replaces the actor pose (sleeping), faded in and out at its edges.
            float const w = std::clamp(camera->viewWeight, 0.0f, 1.0f);
            auto const& v = *camera->view;
            pose.position = {
                std::lerp(pose.position.x, v.position.x, w),
                std::lerp(pose.position.y, v.position.y, w),
                std::lerp(pose.position.z, v.position.z, w),
            };
            pose.pitch = interpolateAngle(pose.pitch, v.pitch, w);
            pose.yaw   = interpolateAngle(pose.yaw, v.yaw, w);
        }
    }
    return pose;
}

float sampleReplaySubjectViewWeight(ReplaySampleTime const& sample) {
    if (!isReplaySubjectRecorder()) {
        // No recorded bed camera. Sleeping only publishes the fade the vignette and the hand already read.
        float const weight = replaySubjectPostureViewWeight();
        gSubjectViewWeight.store(weight, std::memory_order_relaxed);
        gSubjectSleepTicks.store(replaySubjectPostureSleepTicks(), std::memory_order_relaxed);
        return weight;
    }
    auto const camera = sampleSubjectCamera(sample);
    float const weight = camera && camera->view ? camera->viewWeight : 0.0f;
    gSubjectViewWeight.store(weight, std::memory_order_relaxed);
    gSubjectSleepTicks.store(camera && camera->view ? camera->sleepTicks : 0.0f, std::memory_order_relaxed);
    return weight;
}

std::optional<float> sampleReplaySubjectFov(ReplaySampleTime const& sample) {
    if (!isReplaySubjectRecorder()) return 70.0f;
    auto const camera = sampleSubjectCamera(sample);
    if (camera && camera->fov > 0.0f) return camera->fov;
    return std::nullopt;
}

} // namespace playback::visuals
