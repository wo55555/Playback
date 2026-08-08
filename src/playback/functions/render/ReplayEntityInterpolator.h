#pragma once

#include "playback/functions/render/ReplaySampleTime.h"

#include <cstdint>
#include <memory>
#include <vector>

class Actor;
class StrictEntityContext;
struct RenderPositionComponent;

namespace playback::functions::render {

struct EntityRenderKey {
    uint32_t registryId{};
    uint32_t entityId{};

    bool operator==(EntityRenderKey const&) const = default;
};

struct EntityRenderPosition {
    float x{};
    float y{};
    float z{};
};

struct EntityRenderPose {
    EntityRenderPosition position;
    float                pitch{};
    float                yaw{};
    float                headYaw{};
    float                bodyYaw{};
};

struct EntityRenderTarget {
    EntityRenderKey key;
    Actor*          actor{};
};

class ScopedReplayEntityPose;

[[nodiscard]] std::unique_ptr<ScopedReplayEntityPose>
createReplayEntityRenderScope(std::vector<EntityRenderTarget> const& targets, ReplaySampleTime const& sample);

void queueReplayEntityPose(EntityRenderKey key, EntityRenderPose previous, EntityRenderPose current);
void commitReplayEntityPoses(int64_t tick);
void removeReplayEntityPose(EntityRenderKey key);
void clearReplayEntityPoses();

void reapplyReplayEntityPosition(RenderPositionComponent& position) noexcept;

class ScopedReplayEntityPose {
public:
    ~ScopedReplayEntityPose();

    ScopedReplayEntityPose(ScopedReplayEntityPose const&)            = delete;
    ScopedReplayEntityPose& operator=(ScopedReplayEntityPose const&) = delete;

    [[nodiscard]] bool applied() const noexcept { return mState != nullptr; }

private:
    struct State;

    explicit ScopedReplayEntityPose(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> mState;

    friend std::unique_ptr<ScopedReplayEntityPose>
    createReplayEntityRenderScope(std::vector<EntityRenderTarget> const&, ReplaySampleTime const&);
    friend void reapplyReplayEntityPosition(RenderPositionComponent&) noexcept;
};

} // namespace playback::functions::render
