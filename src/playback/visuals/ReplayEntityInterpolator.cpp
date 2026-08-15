#include "ReplayEntityInterpolator.h"

#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/entity/components/RenderPositionComponent.h"
#include "mc/entity/components/RenderRotationComponent.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/BuiltInActorComponents.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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
    int64_t         previousTick{};
    int64_t         currentTick{};
    EntityRenderPose previous{};
    EntityRenderPose current{};
};

struct PendingEntityPose {
    EntityRenderPose previous{};
    EntityRenderPose current{};
};

using EntityPoseMap = std::unordered_map<EntityRenderKey, EntityPoseHistory, EntityRenderKeyHash>;

std::atomic<std::shared_ptr<EntityPoseMap const>> gEntityPoses;
std::mutex                                         gPendingMutex;
std::unordered_map<EntityRenderKey, PendingEntityPose, EntityRenderKeyHash> gPending;

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

} // namespace

struct ScopedReplayEntityPose::State {
    struct ActorState {
        RenderPositionComponent*    renderPosition{};
        Vec3                        renderPositionValue{};
        Vec3                        exportPosition{};
        RenderRotationComponent*    renderRotation{};
        Vec2                        renderRotationValue{};
        ActorRotationComponent*     actorRotation{};
        Vec2                        actorRotationValue{};
        Vec2                        actorRotationPreviousValue{};
        ActorHeadRotationComponent* headRotation{};
        float                       headYaw{};
        float                       previousHeadYaw{};
        MobBodyRotationComponent*   bodyRotation{};
        float                       bodyYaw{};
        float                       previousBodyYaw{};
    };

    std::vector<ActorState> actors;
    State*                  previous{};

    ~State() {
        if (gActiveState == this) gActiveState = previous;
        for (auto state = actors.rbegin(); state != actors.rend(); ++state) {
            state->renderPosition->mValue = state->renderPositionValue;
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

std::unique_ptr<ScopedReplayEntityPose>
createReplayEntityRenderScope(std::vector<EntityRenderTarget> const& targets, ReplaySampleTime const& sample) {
    if (!sample.isValid() || ScopedReplayEntityPose::State::gActiveState) return {};

    try {
        auto const poses = gEntityPoses.load(std::memory_order_acquire);
        auto       state = std::make_unique<ScopedReplayEntityPose::State>();
        state->actors.reserve(targets.size());

        for (auto const& target : targets) {
            if (!target.actor) return {};
            if (!poses) continue;

            auto const history = poses->find(target.key);
            if (history == poses->end()) continue;

            auto& context        = target.actor->getEntityContext();
            auto* renderPosition = context.tryGetComponent<RenderPositionComponent>().as_ptr();
            auto* renderRotation = context.tryGetComponent<RenderRotationComponent>().as_ptr();
            auto* actorRotation  = target.actor->mBuiltInComponents->mActorRotationComponent.get();
            auto* headRotation   = context.tryGetComponent<ActorHeadRotationComponent>().as_ptr();
            auto* bodyRotation   = context.tryGetComponent<MobBodyRotationComponent>().as_ptr();
            if (!renderPosition || !actorRotation) return {};

            auto const sampled = samplePose(history->second, sample);
            state->actors.emplace_back(ScopedReplayEntityPose::State::ActorState{
                renderPosition,
                renderPosition->mValue.get(),
                Vec3{sampled.position.x, sampled.position.y, sampled.position.z},
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
            });

            auto& applied = state->actors.back();
            applied.renderPosition->mValue = applied.exportPosition;
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

        state->previous                         = ScopedReplayEntityPose::State::gActiveState;
        ScopedReplayEntityPose::State::gActiveState = state.get();
        return std::unique_ptr<ScopedReplayEntityPose>(
            new ScopedReplayEntityPose(std::move(state))
        );
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

void commitReplayEntityPoses(int64_t tick) {
    if (tick < 0) return;
    std::scoped_lock lock(gPendingMutex);
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
    gEntityPoses.store({}, std::memory_order_release);
}

void reapplyReplayEntityPosition(RenderPositionComponent& position) noexcept {
    auto* state = ScopedReplayEntityPose::State::gActiveState;
    if (!state) return;
    for (auto const& actor : state->actors) {
        if (actor.renderPosition != &position) continue;
        position.mValue = actor.exportPosition;
        return;
    }
}

} // namespace playback::visuals
