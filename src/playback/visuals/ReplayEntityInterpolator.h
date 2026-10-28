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

// Empty subject choice is the recorder. Other players only get a pose, a hand and a hidden model.
void setReplaySubjectRecorder(bool recorder) noexcept;

[[nodiscard]] bool isReplaySubjectRecorder() noexcept;

// A non-player subject (any followed entity) records its feet, so the view adds its eye height and looks along its
// head yaw. It gets no hand and no HUD. Reset to a player by clearing the subject.
void setReplaySubjectEntity(bool player, float eyeHeight) noexcept;

[[nodiscard]] bool isReplaySubjectPlayer() noexcept;

[[nodiscard]] bool hasReplaySubject() noexcept;

// Flags of the followed replay actor, read once per replay tick. Other players have no recorded camera
// offset, so sneak and swim use the offsets measured on the local player. Crawl is recorded but not applied.
struct ReplaySubjectPosture {
    bool sneaking{};
    bool swimming{};
    bool crawling{};
    bool sleeping{};
    bool actorSleeping{};
};

// snap resets the smoothed offset.
void noteReplaySubjectPosture(int64_t tick, bool snap, ReplaySubjectPosture const& posture);
[[nodiscard]] float replaySubjectPostureOffsetY() noexcept;
[[nodiscard]] float replaySubjectPostureViewWeight() noexcept;
[[nodiscard]] float replaySubjectPostureSleepTicks() noexcept;

[[nodiscard]] bool isReplaySubject(EntityRenderKey const& key) noexcept;

// Independent reasons to hide the subject's model; it stays hidden while any is set.
// Session only owns the render hooks for the whole replay and never hides anything (hooking is slow).
// Export and Preview are set per frame while a first-person sequence segment renders.
enum class SubjectHideReason : uint8_t {
    Export  = 1,
    Follow  = 2,
    Session = 4,
    Preview = 8,
};

// While hidden, the subject's model is not drawn by any render scope (a first-person view sits inside it).
void setReplaySubjectHidden(SubjectHideReason reason, bool hidden);

[[nodiscard]] bool isReplaySubjectHiddenFor(SubjectHideReason reason);

// True while the follow view owns the camera (the observer sits on the recorded player).
[[nodiscard]] bool isReplaySubjectFollowed();

// While set, the follow view sits behind the subject: its model is drawn and the first-person hand is not.
void setReplaySubjectThirdPerson(bool thirdPerson);

// The third-person choice, unless the subject is asleep (its recorded bed view is first person).
[[nodiscard]] bool isReplaySubjectThirdPerson();

// True while the subject's view drives the camera: follow preview or a first-person segment (preview or export).
[[nodiscard]] bool isReplaySubjectViewDriven();

// True for the actor the active render scope on this thread is hiding.
[[nodiscard]] bool isReplayEntityHiddenInScope(Actor const& actor);

// The actor the active render scope on this thread is hiding, or null.
[[nodiscard]] Actor* replayHiddenActorInScope();

// Every signal that can say the hidden subject is asleep; the synthetic actor's own flags are not proven reliable.
struct SubjectSleepSignals {
    bool  actorSleeping{};  // Actor::isSleeping()
    bool  statusSleeping{}; // ActorFlags::Sleeping
    float viewWeight{};     // blend of the recorded sleeping camera view

    [[nodiscard]] bool sleeping() const { return actorSleeping || statusSleeping || viewWeight > 0.0f; }
};

// Sleep state of the subject the active render scope hides; empty without such a scope on this thread.
[[nodiscard]] std::optional<SubjectSleepSignals> replaySubjectSleepInScope();

// Blend of the followed subject's recorded sleeping view at the last sample (0 when not following); readable from the
// HUD pass, which runs outside any render scope.
[[nodiscard]] float replaySubjectViewWeight();

// The subject's consecutive sleeping ticks at the last sample; drives the replayed sleep fade-to-black.
[[nodiscard]] float replaySubjectSleepTicks();

// The followed subject's nausea, refreshed by every render scope that hides it for a follow view.
struct SubjectNauseaSample {
    bool        present{};  // the subject has the effect at all
    int         duration{}; // remaining ticks, -1 when infinite
    bool        active{};   // long enough to keep the distortion growing
    long double tick{};     // replay time of the sample
};

// Empty unless a follow render scope refreshed it within the last half second.
[[nodiscard]] std::optional<SubjectNauseaSample> sampleReplaySubjectNausea();

// Camera position minus entity position of the subject plus the rendered FOV in degrees (0 when the renderer had none
// yet), committed with the next commitReplayEntityPoses. A view (position, yaw, pitch) replaces the whole first-person
// pose for poses the offset cannot express, such as the vanilla sleeping camera.
void queueReplaySubjectCameraOffset(
    EntityRenderPosition            offset,
    float                           fov,
    std::optional<EntityRenderPose> view = std::nullopt
);

// The subject's entity pose with the recorded camera offset added to its position, i.e. the first-person view.
[[nodiscard]] std::optional<EntityRenderPose> sampleReplaySubjectPose(ReplaySampleTime const& sample);

// How much of the recorded game camera view (sleeping) replaces the entity pose, 0 to 1.
[[nodiscard]] float sampleReplaySubjectViewWeight(ReplaySampleTime const& sample);

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
    friend bool   isReplayEntityHiddenInScope(Actor const&);
    friend Actor* replayHiddenActorInScope();
};

} // namespace playback::visuals
