#include "ReplayEntityInterpolator.h"

#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/vanilla_components/AABBShapeComponent.h"
#include "mc/deps/vanilla_components/ActorDataSeatOffsetComponent.h"
#include "mc/deps/vanilla_components/OffsetsComponent.h"
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
#include "mc/world/actor/provider/SynchedActorDataAccess.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffect.h"
#include "mc/world/effect/MobEffectIds.h"
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

// Frame samples of one entity for the pose interval ending at tick.
struct LocalViewHistory {
    EntityRenderKey              key{};
    int64_t                      tick{};
    std::vector<LocalViewSample> samples; // sorted by fraction, all inside (0, 1)
};

using EntityPoseMap       = std::unordered_map<EntityRenderKey, EntityPoseHistory, EntityRenderKeyHash>;
using RidingAttachmentMap = std::unordered_map<EntityRenderKey, ReplayRidingAttachment, EntityRenderKeyHash>;

std::atomic<std::shared_ptr<EntityPoseMap const>>                           gEntityPoses;
std::atomic<std::shared_ptr<RidingAttachmentMap const>>                     gRidingAttachments;
std::atomic<std::shared_ptr<CameraOffsetHistory const>>                     gCameraOffset;
std::mutex                                                                  gPendingMutex;
std::unordered_map<EntityRenderKey, PendingEntityPose, EntityRenderKeyHash> gPending;
std::optional<PendingCameraOffset>                                          gPendingCameraOffset;
std::atomic<std::shared_ptr<LocalViewHistory const>>                        gLocalView;
std::optional<LocalViewHistory>                                             gPendingLocalView;

constexpr uint64_t   kNoSubject = std::numeric_limits<uint64_t>::max();
std::atomic_uint64_t gSubject{kNoSubject};
std::atomic_bool     gSubjectIsRecorder{true};
std::atomic_bool     gSubjectIsPlayer{true};
std::atomic<float>   gSubjectEyeHeight{0.0f}; // feet to eyes of a non-player subject
std::atomic_uint8_t  gSubjectHidden{0};
// A third-person follow view sits behind the subject, so the follow reason no longer hides its model.
std::atomic_bool     gSubjectThirdPerson{false};
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
std::atomic_bool    gNauseaInsidePortal{false};

int64_t steadyNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void publishSubjectNausea(Actor const& subject, long double tick) {
    // 26.40 has no MobEffect::CONFUSION accessor; look the effect up by id.
    auto const* effect = subject.getEffect(static_cast<uint>(MobEffectIds::Confusion));
    int const   duration  = effect ? effect->mDuration->mValue : 0;
    gNauseaPresent.store(effect != nullptr, std::memory_order_relaxed);
    gNauseaDuration.store(duration, std::memory_order_relaxed);
    gNauseaActive.store(effect && (duration == -1 || duration > 60), std::memory_order_relaxed);
    gNauseaTick.store(static_cast<double>(tick), std::memory_order_relaxed);
    gNauseaInsidePortal.store(subject.isInsidePortal(), std::memory_order_relaxed);
    gNauseaStamp.store(steadyNs(), std::memory_order_release);
}

bool finite(EntityRenderPosition const& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(EntityRenderPose const& value) noexcept {
    return finite(value.position) && std::isfinite(value.pitch) && std::isfinite(value.yaw)
        && std::isfinite(value.headYaw) && std::isfinite(value.bodyYaw);
}

bool finite(Vec3 const& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(ReplayRidingAttachment const& value) noexcept {
    return finite(value.seatOffset) && std::isfinite(value.vehicleTickYaw);
}

bool sameAttachment(ReplayRidingAttachment const& left, ReplayRidingAttachment const& right) noexcept {
    return left.vehicleKey == right.vehicleKey && left.seatOffset.x == right.seatOffset.x
        && left.seatOffset.y == right.seatOffset.y && left.seatOffset.z == right.seatOffset.z
        && left.vehicleTickYaw == right.vehicleTickYaw;
}

// Minecraft yaw increases clockwise around +Y (forward is (-sin(yaw), cos(yaw))), so rotating (x, z) by deltaYaw
// gives (x*cos - z*sin, x*sin + z*cos).
Vec3 rotateHorizontalOffset(Vec3 const& offset, float deltaYawDegrees) noexcept {
    if (!std::isfinite(deltaYawDegrees) || std::abs(deltaYawDegrees) < 1e-5f) return offset;
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    float const     rad       = deltaYawDegrees * kDegToRad;
    float const     c         = std::cos(rad);
    float const     s         = std::sin(rad);
    return Vec3{offset.x * c - offset.z * s, offset.y, offset.x * s + offset.z * c};
}

struct RidingAttachmentUpdate {
    EntityRenderKey                       key{};
    std::optional<ReplayRidingAttachment> attachment;
};

void syncRidingAttachments(std::vector<RidingAttachmentUpdate> const& updates) {
    if (updates.empty()) return;
    auto const current = gRidingAttachments.load(std::memory_order_acquire);
    bool       changed = false;
    for (auto const& update : updates) {
        if (!update.attachment) {
            if (current && current->contains(update.key)) {
                changed = true;
                break;
            }
        } else {
            if (!current) {
                changed = true;
                break;
            }
            auto const it = current->find(update.key);
            if (it == current->end() || !sameAttachment(it->second, *update.attachment)) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;

    std::scoped_lock lock(gPendingMutex);
    auto const       latest = gRidingAttachments.load(std::memory_order_acquire);
    auto next = latest ? std::make_shared<RidingAttachmentMap>(*latest) : std::make_shared<RidingAttachmentMap>();
    for (auto const& update : updates) {
        if (update.attachment && finite(*update.attachment)) {
            next->insert_or_assign(update.key, *update.attachment);
        } else {
            next->erase(update.key);
        }
    }
    gRidingAttachments.store(std::move(next), std::memory_order_release);
}

std::optional<ReplayRidingAttachment>
resolveActorRidingAttachment(Actor const& actor, std::unordered_map<Actor const*, EntityRenderKey> const& actorKeys) {
    Actor const* vehicle = actor.getVehicle();
    if (!vehicle) return std::nullopt;
    auto const keyIt = actorKeys.find(vehicle);
    if (keyIt == actorKeys.end()) return std::nullopt;

    if (auto const* seatOffsetComp =
            actor.getEntityContext().tryGetComponent<ActorDataSeatOffsetComponent>().as_ptr()) {
        Vec3 const seatOffset = seatOffsetComp->mValue;
        if (finite(seatOffset) && (seatOffset.x != 0.0f || seatOffset.y != 0.0f || seatOffset.z != 0.0f)) {
            return ReplayRidingAttachment{keyIt->second, seatOffset, 0.0f};
        }
    }

    auto const* stateVector        = actor.mBuiltInComponents->mStateVectorComponent.get();
    auto const* vehicleStateVector = vehicle->mBuiltInComponents->mStateVectorComponent.get();
    auto const* vehicleRotation    = vehicle->mBuiltInComponents->mActorRotationComponent.get();
    if (!stateVector || !vehicleStateVector || !vehicleRotation) return std::nullopt;

    Vec3 const origPos        = stateVector->mPos.get();
    Vec3 const vehicleOrigPos = vehicleStateVector->mPos.get();
    Vec3 const rel{
        origPos.x - vehicleOrigPos.x,
        origPos.y - vehicleOrigPos.y,
        origPos.z - vehicleOrigPos.z,
    };
    ReplayRidingAttachment const attachment{
        keyIt->second,
        rel,
        static_cast<float>(vehicleRotation->mRot.get().y),
    };
    if (!finite(attachment)) return std::nullopt;
    return attachment;
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

// Replaces the straight-line pitch and yaw with the recorded frames when the sample falls in their tick.
void applyLocalView(
    EntityRenderPose&        pose,
    LocalViewHistory const*  view,
    EntityRenderKey const&   key,
    EntityPoseHistory const& history,
    ReplaySampleTime const&  sample
) noexcept {
    if (!view || !(view->key == key) || view->samples.empty()) return;
    int64_t const start  = std::max<int64_t>(0, view->tick - 1);
    float const   amount = static_cast<float>(sample.value() - static_cast<long double>(start));
    if (!(amount > 0.0f && amount < 1.0f)) return;

    auto const from             = samplePose(history, ReplaySampleTime{start, 1});
    auto const to               = samplePose(history, ReplaySampleTime{view->tick, 1});
    float      previousFraction = 0.0f;
    float      previousPitch    = from.pitch;
    float      previousYaw      = from.yaw;
    for (auto const& frame : view->samples) {
        if (amount <= frame.fraction) {
            float const local = (amount - previousFraction) / (frame.fraction - previousFraction);
            pose.pitch        = interpolateAngle(previousPitch, frame.pitch, local);
            pose.yaw          = interpolateAngle(previousYaw, frame.yaw, local);
            return;
        }
        previousFraction = frame.fraction;
        previousPitch    = frame.pitch;
        previousYaw      = frame.yaw;
    }
    float const local = (amount - previousFraction) / (1.0f - previousFraction);
    pose.pitch        = interpolateAngle(previousPitch, to.pitch, local);
    pose.yaw          = interpolateAngle(previousYaw, to.yaw, local);
}

std::optional<EntityRenderPosition> resolveRidingPosition(
    EntityRenderKey            key,
    EntityPoseMap const&       poses,
    RidingAttachmentMap const& attachments,
    ReplaySampleTime const&    sample
) noexcept {
    ReplayRidingAttachment const* chain[16]{};
    size_t                        depth  = 0;
    EntityRenderKey               cursor = key;
    while (depth < 16) {
        auto const it = attachments.find(cursor);
        if (it == attachments.end()) break;
        chain[depth++] = &it->second;
        cursor         = it->second.vehicleKey;
        if (cursor == key) return std::nullopt;
    }
    if (depth == 0) return std::nullopt;

    auto const rootHistory = poses.find(cursor);
    if (rootHistory == poses.end()) return std::nullopt;
    auto                 rootPose = samplePose(rootHistory->second, sample);
    EntityRenderPosition pos      = rootPose.position;
    float                yaw      = rootPose.yaw;

    for (size_t i = depth; i > 0; --i) {
        auto const* att      = chain[i - 1];
        float const deltaYaw = std::remainder(yaw - att->vehicleTickYaw, 360.0f);
        Vec3 const  rotated  = rotateHorizontalOffset(att->seatOffset, deltaYaw);
        pos                  = EntityRenderPosition{pos.x + rotated.x, pos.y + rotated.y, pos.z + rotated.z};
        if (i > 1) {
            auto const nextHistory = poses.find(chain[i - 2]->vehicleKey);
            if (nextHistory == poses.end()) return std::nullopt;
            yaw = samplePose(nextHistory->second, sample).yaw;
        }
    }
    return pos;
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
    float viewWeight = previous.view && current.view ? 1.0f
                     : current.view                  ? amount
                     : previous.view                 ? 1.0f - amount
                                                     : 0.0f;
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
        AABBShapeComponent*            aabbShape{};
        AABB                           aabbValue{};
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
            auto& context = hidden->actor->getEntityContext();
            SynchedActorDataAccess::setActorFlag(context, ActorFlags::HiddenWhenInvisible, hidden->hiddenWhenInvisible);
            SynchedActorDataAccess::setActorFlag(context, ActorFlags::Invisible, hidden->invisible);
        }
        for (auto state = actors.rbegin(); state != actors.rend(); ++state) {
            state->stateVector->mPos      = state->statePosition;
            state->stateVector->mPosPrev  = state->statePositionPrevious;
            state->stateVector->mPosDelta = state->statePositionDelta;
            if (state->aabbShape) state->aabbShape->mAABB = state->aabbValue;
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
    if (!state || !state->hidden) return false;
    if (state->hidden->actor == &actor) return true;
    // Hide passengers (such as server disguise blocks) riding the first-person subject.
    int depth = 0;
    for (auto const* vehicle = actor.getVehicle(); vehicle && depth < 16; vehicle = vehicle->getVehicle(), ++depth) {
        if (vehicle == state->hidden->actor) return true;
    }
    return false;
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
        gNauseaInsidePortal.load(std::memory_order_relaxed),
    };
}

std::unique_ptr<ScopedReplayEntityPose>
createReplayEntityRenderScope(std::vector<EntityRenderTarget> const& targets, ReplaySampleTime const& sample) {
    if (!sample.isValid() || ScopedReplayEntityPose::State::gActiveState) return {};

    try {
        auto const poses = gEntityPoses.load(std::memory_order_acquire);
        auto const localView = gLocalView.load(std::memory_order_acquire);
        auto       state = std::make_unique<ScopedReplayEntityPose::State>();
        state->actors.reserve(targets.size());

        // Frames right after a dimension switch have no committed poses yet; the hidden subject must still be hidden.
        uint8_t hiddenMask = gSubjectHidden.load(std::memory_order_acquire);
        if (isReplaySubjectThirdPerson()) {
            hiddenMask &= static_cast<uint8_t>(
                ~(static_cast<uint8_t>(SubjectHideReason::Follow) | static_cast<uint8_t>(SubjectHideReason::Session))
            );
        }
        auto const subject = hiddenMask != 0 ? subjectKey() : std::nullopt;
        std::unordered_map<Actor const*, EntityRenderKey> actorKeys;
        actorKeys.reserve(targets.size());
        std::vector<EntityRenderTarget const*> nonRidingTargets;
        std::vector<EntityRenderTarget const*> ridingTargets;
        nonRidingTargets.reserve(targets.size());
        ridingTargets.reserve(targets.size());

        for (auto const& target : targets) {
            // A despawned or unloaded entity must not cost the rest of the frame its interpolation.
            if (!target.actor) continue;
            actorKeys.emplace(target.actor, target.key);

            if (subject && target.key == *subject && !state->hidden) {
                state->hidden = ScopedReplayEntityPose::State::HiddenActor{
                    target.actor,
                    target.actor->getStatusFlag(ActorFlags::Invisible),
                    target.actor->getStatusFlag(ActorFlags::HiddenWhenInvisible),
                };
                auto& context = target.actor->getEntityContext();
                SynchedActorDataAccess::setActorFlag(context, ActorFlags::Invisible, true);
                SynchedActorDataAccess::setActorFlag(context, ActorFlags::HiddenWhenInvisible, true);
                publishSubjectNausea(*target.actor, sample.value());
            }

            if (target.actor->getVehicle() != nullptr) ridingTargets.push_back(&target);
            else nonRidingTargets.push_back(&target);
        }

        if (poses) {
            struct VehicleDelta {
                Vec3  origStatePos{};
                Vec3  newExportPos{};
                float origYaw{0.0f};
                float newYaw{0.0f};
            };

            std::unordered_map<Actor const*, VehicleDelta> vehicleDeltas;
            vehicleDeltas.reserve(targets.size());
            std::vector<RidingAttachmentUpdate> attachmentUpdates;
            attachmentUpdates.reserve(targets.size());

            auto const applyTarget = [&](EntityRenderTarget const& target, std::optional<Vec3> const& lockedPos) {
                auto const history = poses->find(target.key);
                if (history == poses->end()) return false;

                auto& context              = target.actor->getEntityContext();
                auto* renderPosition       = context.tryGetComponent<RenderPositionComponent>().as_ptr();
                auto* renderRotation       = context.tryGetComponent<RenderRotationComponent>().as_ptr();
                auto* movementInterpolator = context.tryGetComponent<MovementInterpolatorComponent>().as_ptr();
                auto* stateVector          = target.actor->mBuiltInComponents->mStateVectorComponent.get();
                auto* aabbShape            = target.actor->mBuiltInComponents->mAABBShapeComponent.get();
                auto* actorRotation        = target.actor->mBuiltInComponents->mActorRotationComponent.get();
                auto* headRotation         = context.tryGetComponent<ActorHeadRotationComponent>().as_ptr();
                auto* bodyRotation         = context.tryGetComponent<MobBodyRotationComponent>().as_ptr();
                if (!stateVector || !renderPosition || !actorRotation) return false;

                auto sampled = samplePose(history->second, sample);
                applyLocalView(sampled, localView.get(), target.key, history->second, sample);

                Vec3 const  origStatePos = stateVector->mPos.get();
                float const origYaw      = static_cast<float>(actorRotation->mRot.get().y);
                bool const  positioned   = (target.actor->getVehicle() == nullptr) || lockedPos.has_value();
                Vec3 const  exportPos =
                    lockedPos ? *lockedPos : Vec3{sampled.position.x, sampled.position.y, sampled.position.z};

                state->actors.emplace_back(
                    ScopedReplayEntityPose::State::ActorState{
                        stateVector,
                        origStatePos,
                        stateVector->mPosPrev.get(),
                        stateVector->mPosDelta.get(),
                        aabbShape,
                        aabbShape ? aabbShape->mAABB.get() : AABB{},
                        renderPosition,
                        renderPosition->mValue.get(),
                        exportPos,
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
                    // The hidden subject keeps its per-tick movement for first-person hand bob; a gliding entity
                    // needs its flight velocity for body banking, gliding speed damping, and elytra wing spread.
                    bool const isSubject = state->hidden && state->hidden->actor == target.actor;
                    if (isSubject) {
                        applied.stateVector->mPosDelta = applied.statePositionDelta;
                    } else if (target.actor->getStatusFlag(ActorFlags::Gliding)) {
                        Vec3 glideDelta = applied.statePositionDelta;
                        if (history->second.currentTick > history->second.previousTick) {
                            float const invTicks =
                                1.0f / static_cast<float>(history->second.currentTick - history->second.previousTick);
                            Vec3 const sampledDelta{
                                (history->second.current.position.x - history->second.previous.position.x) * invTicks,
                                (history->second.current.position.y - history->second.previous.position.y) * invTicks,
                                (history->second.current.position.z - history->second.previous.position.z) * invTicks,
                            };
                            if (std::isfinite(sampledDelta.x) && std::isfinite(sampledDelta.y)
                                && std::isfinite(sampledDelta.z)
                                && (sampledDelta.x != 0.0f || sampledDelta.y != 0.0f || sampledDelta.z != 0.0f)) {
                                glideDelta = sampledDelta;
                            }
                        }
                        applied.stateVector->mPosDelta = glideDelta;
                    } else {
                        applied.stateVector->mPosDelta = Vec3{};
                    }
                    applied.renderPosition->mValue = applied.exportPosition;
                    vehicleDeltas.emplace(
                        target.actor,
                        VehicleDelta{origStatePos, applied.exportPosition, origYaw, sampled.yaw}
                    );
                }
                if (applied.movementInterpolator) {
                    if (positioned) *applied.movementInterpolator->mPos = applied.exportPosition;
                    *applied.movementInterpolator->mRot          = Vec2{sampled.pitch, sampled.yaw};
                    applied.movementInterpolator->mHeadYaw       = sampled.headYaw;
                    applied.movementInterpolator->mPositionSteps = 0;
                    applied.movementInterpolator->mRotationSteps = 0;
                    applied.movementInterpolator->mHeadYawSteps  = 0;
                }
                // Replay positions are written straight into the state, so the bounding box the game moves along with
                // them stays wherever the entity was spawned or last teleported. The fire overlay, culling and the
                // viewport's entity picking read it, so it follows the drawn pose and restores when the scope ends.
                if (applied.aabbShape) {
                    Vec3 const position = positioned ? applied.exportPosition : Vec3{stateVector->mPos};
                    syncReplayEntityBoundingBox(*target.actor, position);
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
                return true;
            };

            std::unordered_map<EntityRenderKey, std::optional<ReplayRidingAttachment>, EntityRenderKeyHash>
                resolvedAttachments;
            resolvedAttachments.reserve(ridingTargets.size());
            for (auto const* target : ridingTargets) {
                resolvedAttachments.emplace(target->key, resolveActorRidingAttachment(*target->actor, actorKeys));
            }

            // Pass 1: position all non-riding actors (including vehicles) and record their tick->frame deltas.
            for (auto const* target : nonRidingTargets) {
                attachmentUpdates.push_back({target->key, std::nullopt});
                (void)applyTarget(*target, std::nullopt);
            }

            // Pass 2: lock riding passengers to their vehicle's interpolated position and yaw delta.
            std::vector<EntityRenderTarget const*> pendingRiding = std::move(ridingTargets);
            while (!pendingRiding.empty()) {
                std::vector<EntityRenderTarget const*> remaining;
                bool                                   progressed = false;
                for (auto const* target : pendingRiding) {
                    Actor const* vehicle = target->actor->getVehicle();
                    auto const   it      = vehicle ? vehicleDeltas.find(vehicle) : vehicleDeltas.end();
                    if (it == vehicleDeltas.end()) {
                        remaining.push_back(target);
                        continue;
                    }
                    progressed              = true;
                    auto const attachmentIt = resolvedAttachments.find(target->key);
                    auto const attachment =
                        attachmentIt != resolvedAttachments.end() ? attachmentIt->second : std::nullopt;
                    if (!attachment) {
                        attachmentUpdates.push_back({target->key, std::nullopt});
                        (void)applyTarget(*target, std::nullopt);
                        continue;
                    }
                    float const deltaYaw = std::remainder(it->second.newYaw - attachment->vehicleTickYaw, 360.0f);
                    Vec3 const  rotated  = rotateHorizontalOffset(attachment->seatOffset, deltaYaw);
                    Vec3 const  lockedPos{
                        it->second.newExportPos.x + rotated.x,
                        it->second.newExportPos.y + rotated.y,
                        it->second.newExportPos.z + rotated.z,
                    };
                    if (applyTarget(*target, lockedPos)) {
                        attachmentUpdates.push_back({target->key, attachment});
                    } else {
                        attachmentUpdates.push_back({target->key, std::nullopt});
                    }
                }
                if (!progressed) {
                    for (auto const* target : remaining) {
                        attachmentUpdates.push_back({target->key, std::nullopt});
                        (void)applyTarget(*target, std::nullopt);
                    }
                    break;
                }
                pendingRiding = std::move(remaining);
            }
            syncRidingAttachments(attachmentUpdates);
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

void syncReplayEntityBoundingBox(Actor& actor, Vec3 const& position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        return;
    }
    auto* shape = actor.mBuiltInComponents->mAABBShapeComponent.get();
    if (!shape) return;
    auto const* offsets = actor.getEntityContext().tryGetComponent<OffsetsComponent>().as_ptr();
    Vec2 const  size    = shape->mBBDim;
    if (!std::isfinite(size.x) || !std::isfinite(size.y)) return;
    float const width  = std::max(0.0f, static_cast<float>(size.x));
    float const height = std::max(0.0f, static_cast<float>(size.y));
    // Players stand eye height above their box; most entities stand on it.
    float const rawLift = offsets ? static_cast<float>(offsets->mHeightOffset) : 0.0f;
    float const lift    = std::isfinite(rawLift) ? rawLift : 0.0f;
    float const half    = width * 0.5f;
    shape->mAABB        = AABB{
        Vec3{position.x - half, position.y - lift,          position.z - half},
        Vec3{position.x + half, position.y - lift + height, position.z + half}
    };
}

void queueReplayEntityPose(EntityRenderKey key, EntityRenderPose previous, EntityRenderPose current) {
    if (!finite(previous) || !finite(current)) return;
    std::scoped_lock lock(gPendingMutex);
    auto [entry, inserted] = gPending.try_emplace(key, PendingEntityPose{previous, current});
    if (!inserted) entry->second.current = current;
}

void noteReplayEntityRidingAttachment(EntityRenderKey passengerKey, std::optional<ReplayRidingAttachment> attachment) {
    if (attachment && !finite(*attachment)) attachment.reset();
    syncRidingAttachments({
        {passengerKey, attachment}
    });
}

void refreshReplayRidingAttachments(std::vector<EntityRenderTarget> const& targets) {
    if (targets.empty()) return;

    std::unordered_map<Actor const*, EntityRenderKey> actorKeys;
    actorKeys.reserve(targets.size());
    for (auto const& target : targets) {
        if (target.actor) actorKeys.emplace(target.actor, target.key);
    }
    if (actorKeys.empty()) return;

    std::vector<RidingAttachmentUpdate> updates;
    updates.reserve(actorKeys.size());
    for (auto const& target : targets) {
        if (!target.actor) continue;
        updates.push_back({target.key, resolveActorRidingAttachment(*target.actor, actorKeys)});
    }
    syncRidingAttachments(updates);
}

void queueReplaySubjectCameraOffset(EntityRenderPosition offset, float fov, std::optional<EntityRenderPose> view) {
    if (!finite(offset)) return;
    if (view && !finite(*view)) view.reset();
    CameraOffsetSample const
        sample{offset, std::isfinite(fov) && fov > 1.0f && fov < 179.0f ? fov : 0.0f, view, view ? 1.0f : 0.0f};
    std::scoped_lock         lock(gPendingMutex);
    if (gPendingCameraOffset) gPendingCameraOffset->current = sample;
    else gPendingCameraOffset = PendingCameraOffset{sample, sample};
}

void queueReplayLocalViewSamples(EntityRenderKey key, std::vector<LocalViewSample> samples) {
    std::erase_if(samples, [](LocalViewSample const& value) {
        return !(value.fraction > 0.0f && value.fraction < 1.0f) || !std::isfinite(value.pitch)
            || !std::isfinite(value.yaw);
    });
    std::ranges::stable_sort(samples, {}, &LocalViewSample::fraction);
    auto const duplicates = std::ranges::unique(samples, {}, &LocalViewSample::fraction);
    samples.erase(duplicates.begin(), duplicates.end());
    std::scoped_lock lock(gPendingMutex);
    if (samples.empty()) gPendingLocalView.reset();
    else gPendingLocalView = LocalViewHistory{key, 0, std::move(samples)};
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
    // Each tick replaces the frames; a tick without them interpolates in a straight line.
    if (gPendingLocalView) {
        gPendingLocalView->tick = tick;
        gLocalView.store(
            std::make_shared<LocalViewHistory const>(std::move(*gPendingLocalView)),
            std::memory_order_release
        );
        gPendingLocalView.reset();
    } else if (gLocalView.load(std::memory_order_acquire)) {
        gLocalView.store({}, std::memory_order_release);
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
    if (gPendingLocalView && gPendingLocalView->key == key) gPendingLocalView.reset();
    if (auto const view = gLocalView.load(std::memory_order_acquire); view && view->key == key) {
        gLocalView.store({}, std::memory_order_release);
    }
    if (auto const attachments = gRidingAttachments.load(std::memory_order_acquire)) {
        bool const hasKey = attachments->contains(key) || std::ranges::any_of(*attachments, [&](auto const& entry) {
                                return entry.second.vehicleKey == key;
                            });
        if (hasKey) {
            auto nextAttachments = std::make_shared<RidingAttachmentMap>(*attachments);
            std::erase_if(*nextAttachments, [&](auto const& entry) {
                return entry.first == key || entry.second.vehicleKey == key;
            });
            gRidingAttachments.store(std::move(nextAttachments), std::memory_order_release);
        }
    }
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
    gPendingLocalView.reset();
    gLocalView.store({}, std::memory_order_release);
    gRidingAttachments.store({}, std::memory_order_release);
    gEntityPoses.store({}, std::memory_order_release);
}

void setReplaySubject(std::optional<EntityRenderKey> key) {
    if (!key) {
        gSubjectIsRecorder.store(true, std::memory_order_release);
        gSubjectIsPlayer.store(true, std::memory_order_release);
        gSubjectEyeHeight.store(0.0f, std::memory_order_relaxed);
        resetPosture();
    }
    gSubject.store(
        key ? (static_cast<uint64_t>(key->registryId) << 32U) | key->entityId : kNoSubject,
        std::memory_order_release
    );
}

void noteReplaySubjectPosture(int64_t tick, bool snap, ReplaySubjectPosture const& posture) {
    bool const  recorder = isReplaySubjectRecorder();
    // The measured offsets are a player's; another entity keeps its own eye height.
    bool const  player   = isReplaySubjectPlayer();
    float const target   = !recorder && player && (posture.swimming || posture.crawling || posture.gliding)
                             ? kSwimCameraOffsetY
                         : !recorder && player && posture.sneaking ? kSneakCameraOffsetY
                                                                   : 0.0f;
    bool const  sleeping = player && (posture.sleeping || posture.actorSleeping);
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

void setReplaySubjectRecorder(bool recorder) noexcept { gSubjectIsRecorder.store(recorder, std::memory_order_release); }

bool isReplaySubjectRecorder() noexcept { return gSubjectIsRecorder.load(std::memory_order_acquire); }

void setReplaySubjectEntity(bool player, float eyeHeight) noexcept {
    gSubjectEyeHeight.store(std::isfinite(eyeHeight) ? std::max(0.0f, eyeHeight) : 0.0f, std::memory_order_relaxed);
    gSubjectIsPlayer.store(player, std::memory_order_release);
}

bool isReplaySubjectPlayer() noexcept { return gSubjectIsPlayer.load(std::memory_order_acquire); }

bool hasReplaySubject() noexcept { return gSubject.load(std::memory_order_acquire) != kNoSubject; }

bool isReplaySubject(EntityRenderKey const& key) noexcept {
    uint64_t const packed = (static_cast<uint64_t>(key.registryId) << 32U) | key.entityId;
    return gSubject.load(std::memory_order_acquire) == packed;
}

void setReplaySubjectHidden(SubjectHideReason reason, bool hidden) {
    auto const bit = static_cast<uint8_t>(reason);
    constexpr auto viewMask = static_cast<uint8_t>(SubjectHideReason::Follow)
                            | static_cast<uint8_t>(SubjectHideReason::Export)
                            | static_cast<uint8_t>(SubjectHideReason::Preview);
    uint8_t const previous = hidden ? gSubjectHidden.fetch_or(bit, std::memory_order_acq_rel)
                                    : gSubjectHidden.fetch_and(static_cast<uint8_t>(~bit), std::memory_order_acq_rel);
    uint8_t const current  = hidden ? static_cast<uint8_t>(previous | bit) : static_cast<uint8_t>(previous & ~bit);
    // The sleep fade belongs to the recorded view; it restarts once nothing drives the camera from the subject.
    if (reason != SubjectHideReason::Session && !hidden && (current & viewMask) == 0) {
        gSubjectViewWeight.store(0.0f, std::memory_order_relaxed);
        gSubjectSleepTicks.store(0.0f, std::memory_order_relaxed);
    }
}

bool isReplaySubjectHiddenFor(SubjectHideReason reason) {
    return (gSubjectHidden.load(std::memory_order_acquire) & static_cast<uint8_t>(reason)) != 0;
}

bool isReplaySubjectFollowed() { return isReplaySubjectHiddenFor(SubjectHideReason::Follow); }

void setReplaySubjectThirdPerson(bool thirdPerson) {
    gSubjectThirdPerson.store(thirdPerson, std::memory_order_release);
}

// Vanilla drops to first person in bed, so the recorded sleeping view overrides the third-person choice.
bool isReplaySubjectThirdPerson() {
    return gSubjectThirdPerson.load(std::memory_order_acquire)
        && gSubjectViewWeight.load(std::memory_order_relaxed) <= 0.0f;
}

bool isReplaySubjectViewDriven() {
    constexpr auto mask = static_cast<uint8_t>(SubjectHideReason::Follow)
                        | static_cast<uint8_t>(SubjectHideReason::Export)
                        | static_cast<uint8_t>(SubjectHideReason::Preview);
    return (gSubjectHidden.load(std::memory_order_acquire) & mask) != 0;
}

std::optional<EntityRenderPose> sampleReplayEntityPose(
    EntityRenderKey         key,
    ReplaySampleTime const& sample,
    bool                    isRecorder,
    bool                    isPlayer,
    float                   eyeHeight,
    float                   postureOffsetY
) {
    std::optional<EntityRenderPose> found;
    auto const                      poses = gEntityPoses.load(std::memory_order_acquire);
    if (poses) {
        if (auto const entry = poses->find(key); entry != poses->end()) {
            found = samplePose(entry->second, sample);
            applyLocalView(*found, gLocalView.load(std::memory_order_acquire).get(), key, entry->second, sample);
        }
    }
    if (!found) {
        // The seeded pose stays pending until the first tick commits it.
        std::scoped_lock lock(gPendingMutex);
        if (auto const pending = gPending.find(key); pending != gPending.end()) found = pending->second.current;
    }
    if (!found) return std::nullopt;
    auto pose = *found;

    bool attachedToVehicle = false;
    if (poses) {
        if (auto const attachments = gRidingAttachments.load(std::memory_order_acquire)) {
            if (auto const attachedPos = resolveRidingPosition(key, *poses, *attachments, sample)) {
                pose.position     = *attachedPos;
                attachedToVehicle = true;
            }
        }
    }

    // Any other entity records its feet and turns its head independently of its body.
    if (!isPlayer) {
        pose.position.y += std::isfinite(eyeHeight) ? std::max(0.0f, eyeHeight) : 0.0f;
        pose.yaw = pose.headYaw;
        return pose;
    }
    // Recorded player positions are already standing eye height. The recorder uses its recorded camera offset.
    // Another player has no such track, so sneak and swim add the offset measured on the local player.
    if (!isRecorder) {
        if (!attachedToVehicle) {
            pose.position.y += std::isfinite(postureOffsetY) ? postureOffsetY : 0.0f;
        }
        return pose;
    }
    if (auto const camera = sampleSubjectCamera(sample)) {
        if (!attachedToVehicle) {
            pose.position.x += camera->offset.x;
            pose.position.y += camera->offset.y;
            pose.position.z += camera->offset.z;
        }
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

std::optional<EntityRenderPose> sampleReplaySubjectPose(ReplaySampleTime const& sample) {
    auto const subject = subjectKey();
    if (!subject) return std::nullopt;
    return sampleReplayEntityPose(
        *subject,
        sample,
        isReplaySubjectRecorder(),
        isReplaySubjectPlayer(),
        gSubjectEyeHeight.load(std::memory_order_relaxed),
        replaySubjectPostureOffsetY()
    );
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
