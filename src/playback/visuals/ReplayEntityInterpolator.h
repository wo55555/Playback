#pragma once

#include "playback/visuals/ReplaySampleTime.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class Actor;
class StrictEntityContext;
struct RenderPositionComponent;

namespace playback::visuals {

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

// The recorded local player: the entity a first-person view follows. Safe to read from any thread.
void setReplaySubject(std::optional<EntityRenderKey> key);

// Independent reasons to hide the subject's model; it stays hidden while any is set.
enum class SubjectHideReason : uint8_t {
    Export = 1,
    Follow = 2,
};

// While hidden, the subject's model is not drawn by any render scope (a first-person view sits inside it).
void setReplaySubjectHidden(SubjectHideReason reason, bool hidden);

// Camera position minus entity position of the subject plus the rendered FOV in degrees (0 when the renderer had none
// yet), committed with the next commitReplayEntityPoses.
void queueReplaySubjectCameraOffset(EntityRenderPosition offset, float fov);

// The subject's entity pose with the recorded camera offset added to its position, i.e. the first-person view.
[[nodiscard]] std::optional<EntityRenderPose> sampleReplaySubjectPose(ReplaySampleTime const& sample);

// The subject's recorded rendered FOV in degrees; empty until the snapshot or a tick supplies a valid one.
[[nodiscard]] std::optional<float> sampleReplaySubjectFov(ReplaySampleTime const& sample);

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
};

} // namespace playback::visuals
