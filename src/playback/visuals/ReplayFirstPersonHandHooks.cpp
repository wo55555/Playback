#include "ReplayFirstPersonHandHooks.h"

#include "ReplayEntityInterpolator.h"
#include "ReplayHandArm.h"
#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/runtime/GameAddress.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/game/ClientInputCallbacks.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/controls/UIControl.h"
#include "mc/client/gui/controls/renderers/HudVignetteRenderer.h"
#include "mc/client/gui/screens/InGamePlayScreen.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/vanilla_camera/VanillaCameraAPI.h"
#include "mc/deps/vanilla_components/PlayerBobComponent.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components/AbilitiesComponent.h"
#include "mc/entity/components/ActorGameTypeComponent.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/entity/components/ItemInUseComponent.h"
#include "mc/entity/components/WalkDistComponent.h"
#include "mc/entity/systems/VanillaOffsetSystem.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Ability.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/provider/SynchedActorDataAccess.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/item/ItemStack.h"


#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playback::visuals {

namespace {

constexpr int64_t MaxCatchUpTicks = 8;

thread_local int gPassDepth = 0;

Player* subjectPlayer() {
    auto* actor = replayHiddenActorInScope();
    return actor && actor->isPlayer() ? static_cast<Player*>(actor) : nullptr;
}

// The vanilla sleeping camera draws no hand. The synthetic subject's own sleeping flags are not proven, so the
// recorded sleep view counts as well.
bool subjectSleepSuppressesHand() {
    auto const signals = replaySubjectSleepInScope();
    return signals && signals->sleeping();
}

// The vanilla hand mesh only exists for the local player. The pass copies the subject's items onto the observer
// and reports the subject's game type, so a spectator observer still draws that hand and skin.
Player& handPlayer(Player& player) { return player; }

struct HeightTracker {
    Player const* subject{};
    int64_t       tick{-1};
    HandArm       main;
    HandArm       off;
};

std::unique_ptr<HeightTracker> gHeights;

// The SDK leaves HandAnimationState opaque: the hand's item, its hotbar slot (only tracked for the main hand), then
// the current and previous arm heights.
struct HandAnimation {
    ItemStack item;
    int       slot;
    float     height;
    float     oldHeight;
};
static_assert(sizeof(HandAnimation) == sizeof(ItemInHandRenderer::HandAnimationState));

HandAnimation& handAnimation(ItemInHandRenderer& renderer, HandSlot hand) {
    return reinterpret_cast<HandAnimation&>((*renderer.mHandAnimationState)[hand]);
}

// The native renderer follows the local player's items and arm height; the subject's are applied before each pass.
void syncRenderer(ItemInHandRenderer& renderer, Player& subject) {
    auto const& carried = subject.getCarriedItem();
    auto const& offhand = subject.getOffhandSlot();
    int const   slot    = subject.getSelectedItemSlot();

    if (!gHeights) gHeights = std::make_unique<HeightTracker>();
    auto&      state   = *gHeights;
    auto const context = keyframe::currentCameraTimelineRenderContext();
    auto const time    = context ? context->time : ReplaySampleTime{-1, 1};

    float mainHeight = 1.0f;
    float offHeight  = 1.0f;
    if (!time.isValid()) {
        state.tick = -1;
        snapHandArm(state.main, carried, slot);
        snapHandArm(state.off, offhand, HandOffhandSlot);
    } else {
        // Entities render at the tick after the sample time's floor, so the arm state spans [tick - 1, tick].
        int64_t const tick = time.requiredAppliedTick();
        if (state.subject != &subject || state.tick < 0 || tick < state.tick || tick - state.tick > MaxCatchUpTicks) {
            state.subject = &subject;
            state.tick    = tick;
            snapHandArm(state.main, carried, slot);
            snapHandArm(state.off, offhand, HandOffhandSlot);
        }
        while (state.tick < tick) {
            ++state.tick;
            stepHandArm(state.main, carried, slot);
            stepHandArm(state.off, offhand, HandOffhandSlot);
        }
        float const amount =
            std::clamp(static_cast<float>(time.value() - static_cast<long double>(tick - 1)), 0.0f, 1.0f);
        mainHeight = std::lerp(state.main.oldHeight, state.main.height, amount);
        offHeight  = std::lerp(state.off.oldHeight, state.off.height, amount);
    }

    // Old and current heights match so the native alpha cannot change the interpolated value.
    auto& main     = handAnimation(renderer, HandSlot::Mainhand);
    auto& off      = handAnimation(renderer, HandSlot::Offhand);
    main.item      = state.main.item;
    main.slot      = state.main.slot;
    main.height    = mainHeight;
    main.oldHeight = mainHeight;
    off.item       = state.off.item;
    off.height     = offHeight;
    off.oldHeight  = offHeight;
}

// The game's view-bobbing setting; the hand bob follows it like the native first-person view does.
bool viewBobbingEnabled() {
    auto client = ll::service::getClientInstance();
    return client && client->getOptions().getBobView();
}

float wrapDegrees(float degrees) {
    degrees = std::fmod(degrees, 360.0f);
    if (degrees >= 180.0f) degrees -= 360.0f;
    if (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

// InGamePlayScreen::_renderItemInHand sways the hand on a spring driven by the player's turn rate when view
// bobbing is on: angleVel = clamp(0.8 angleVel + 0.2 turn / dt, +-50), then substeps of at
// most 1/120 s of vel += (-900 hand - 42 vel + 90 angleVel) h, hand += h vel; a velocity of 1000 resets it. dt comes
// from a wall clock clamped to 0.2 s and the turn from the observer, which only turns with the subject in live follow
// and ignores pause, seek and the export clock. The same spring runs here on the subject in replay time instead.
struct HandSway {
    Player const* subject{};
    long double   tick{-1.0L};
    Vec2          rotation{};
    Vec2          angleVel{};
    Vec2          hand{};
    Vec2          vel{};
    // Native clock minus steady_clock, in seconds; the smallest observed value is the closest to the true offset.
    double        clockOffset{std::numeric_limits<double>::infinity()};
};

HandSway gSway;

constexpr long double HandSwayResetTicks  = 20.0L;
constexpr double      HandSwayClockMargin = 0.001;

void stepHandSway(HandSway& sway, Vec2 rotation, float dt) {
    Vec2 const turn{wrapDegrees(rotation.x - sway.rotation.x), wrapDegrees(rotation.y - sway.rotation.y)};
    sway.rotation   = rotation;
    sway.angleVel.x = std::clamp(sway.angleVel.x * 0.8f + turn.x / dt * 0.2f, -50.0f, 50.0f);
    sway.angleVel.y = std::clamp(sway.angleVel.y * 0.8f + turn.y / dt * 0.2f, -50.0f, 50.0f);
    for (float left = dt; left > 0.0f;) {
        float const h  = std::min(1.0f / 120.0f, left);
        left          -= h;
        sway.vel.x    += (sway.hand.x * -900.0f + sway.vel.x * -42.0f + sway.angleVel.x * 90.0f) * h;
        sway.vel.y    += (sway.hand.y * -900.0f + sway.vel.y * -42.0f + sway.angleVel.y * 90.0f) * h;
        sway.hand.x   += h * sway.vel.x;
        sway.hand.y   += h * sway.vel.y;
    }
    if (std::abs(sway.vel.x) >= 1000.0f || std::abs(sway.vel.y) >= 1000.0f) sway.vel = Vec2{};
}

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Writes the replay spring into the native fields so the native update only adds a negligible step: the observer's
// rotation is held still for the call and the native clock is set about 1 ms back, so the turn and dt are ~0.
class ScopedHandSway {
public:
    ScopedHandSway(InGamePlayScreen& screen, Player& observer, Player& subject)
    : mScreen(screen),
      mRotation(observer.mBuiltInComponents->mActorRotationComponent.get()) {
        auto*      subjectRotation = subject.mBuiltInComponents->mActorRotationComponent.get();
        auto const sample          = sampleReplaySubjectNausea(); // stamped with the subject's sample time
        if (!mRotation || !subjectRotation || !sample || !viewBobbingEnabled()) {
            mRotation    = nullptr;
            gSway.tick   = -1.0L;
            return;
        }

        auto&      sway     = gSway;
        Vec2 const rotation = subjectRotation->mRot.get();
        if (sway.subject != &subject || sway.tick < 0.0L || sample->tick < sway.tick
            || sample->tick - sway.tick > HandSwayResetTicks) {
            sway.subject  = &subject;
            sway.rotation = rotation;
            sway.angleVel = Vec2{};
            sway.hand     = Vec2{};
            sway.vel      = Vec2{};
        } else if (sample->tick > sway.tick) {
            stepHandSway(sway, rotation, std::min(static_cast<float>((sample->tick - sway.tick) / 20.0L), 0.2f));
        }
        sway.tick = sample->tick;

        mSavedPrevious      = mRotation->mRotPrev.get();
        mRotation->mRotPrev = mRotation->mRot.get();
        mStart              = steadySeconds();
        // The first pass only measures the native clock; its own sway update runs unchanged.
        if (std::isfinite(sway.clockOffset)) {
            screen.mLastHandRotation   = sway.hand;
            screen.mLastHandVel        = Vec2{};
            screen.mLastPlayerAngleVel = Vec2{};
            screen.mLastPlayerRotation = mRotation->mRot.get();
            // A negative native dt is replaced by 0.2 s, so the margin covers the offset's measurement error.
            screen.mLastTime           = mStart + sway.clockOffset - HandSwayClockMargin;
        }
        mWrittenTime = screen.mLastTime;
    }

    ~ScopedHandSway() {
        if (!mRotation) return;
        mRotation->mRotPrev = mSavedPrevious;
        // Only a native update stamps the clock; a skipped one would leave the written value and skew the offset.
        double const stamped = mScreen.mLastTime;
        if (stamped == mWrittenTime) return;
        double const offset = stamped - mStart;
        if (std::isfinite(offset)) gSway.clockOffset = std::min(gSway.clockOffset, offset);
    }

    ScopedHandSway(ScopedHandSway const&)            = delete;
    ScopedHandSway& operator=(ScopedHandSway const&) = delete;

private:
    InGamePlayScreen&       mScreen;
    ActorRotationComponent* mRotation{};
    Vec2                    mSavedPrevious{};
    double                  mStart{};
    double                  mWrittenTime{};
};

// The scope hides the subject with Invisible flags; its hands must render without them.
class ScopedHandPass {
public:
    explicit ScopedHandPass(Player& subject)
    : mSubject(subject),
      mOuter(gPassDepth == 0),
      mInvisible(subject.getStatusFlag(ActorFlags::Invisible)),
      mHiddenWhenInvisible(subject.getStatusFlag(ActorFlags::HiddenWhenInvisible)) {
        if (mOuter) {
            SynchedActorDataAccess::setActorFlag(mSubject.getEntityContext(), ActorFlags::Invisible, false);
            SynchedActorDataAccess::setActorFlag(mSubject.getEntityContext(), ActorFlags::HiddenWhenInvisible, false);

            // The scope leaves the subject's movement in place; with bobbing off it must read as standing still.
            mStateVector = mSubject.mBuiltInComponents->mStateVectorComponent.get();
            if (mStateVector) {
                mPosDelta = mStateVector->mPosDelta.get();
                if (!viewBobbingEnabled()) mStateVector->mPosDelta = Vec3{};
            }
        }
        ++gPassDepth;
    }

    ~ScopedHandPass() {
        --gPassDepth;
        if (mOuter) {
            if (mStateVector) mStateVector->mPosDelta = mPosDelta;
            SynchedActorDataAccess::setActorFlag(
                mSubject.getEntityContext(),
                ActorFlags::HiddenWhenInvisible,
                mHiddenWhenInvisible
            );
            SynchedActorDataAccess::setActorFlag(mSubject.getEntityContext(), ActorFlags::Invisible, mInvisible);
        }
    }

    ScopedHandPass(ScopedHandPass const&)            = delete;
    ScopedHandPass& operator=(ScopedHandPass const&) = delete;

private:
    Player&               mSubject;
    bool                  mOuter;
    bool                  mInvisible;
    bool                  mHiddenWhenInvisible;
    StateVectorComponent* mStateVector{};
    Vec3                  mPosDelta{};
};

LL_TYPE_INSTANCE_HOOK(HandGameTypeHook, ll::memory::HookPriority::High, Player, &Player::getPlayerGameType, GameType) {
    if (gPassDepth > 0 && isLocalPlayer()) {
        auto* subject = subjectPlayer();
        if (subject && subject != this) {
            auto const type = subject->getPlayerGameType();
            if (type != GameType::Spectator && type != GameType::Default && type != GameType::Undefined) return type;
            return GameType::Survival;
        }
    }
    return origin();
}

// 26.51 inlines _showHand into _renderItemInHand: the hand is skipped when the local player's ActorGameTypeComponent
// reads Spectator, or Default under a Spectator level default (1.26.51.01). The observer is a spectator, so the
// component reports the subject's mode for the call.
class ScopedObserverGameType {
public:
    ScopedObserverGameType(Player& observer, Player& subject)
    : mComponent(observer.getEntityContext().tryGetComponent<ActorGameTypeComponent>()) {
        if (!mComponent) return;
        mSaved                = mComponent->mGameType;
        auto const type       = subject.getPlayerGameType();
        mComponent->mGameType = type != GameType::Spectator && type != GameType::Default && type != GameType::Undefined
                                  ? type
                                  : GameType::Survival;
    }

    ~ScopedObserverGameType() {
        if (mComponent) mComponent->mGameType = mSaved;
    }

    ScopedObserverGameType(ScopedObserverGameType const&)            = delete;
    ScopedObserverGameType& operator=(ScopedObserverGameType const&) = delete;

private:
    ActorGameTypeComponent* mComponent;
    GameType                mSaved{};
};

// The hand pass draws the observer, so the subject's swing and item-use state stands in for it during the pass.
class ScopedHandAction {
public:
    ScopedHandAction(Player& observer, Player& subject) : mObserver(observer) {
        mAttackAnim   = observer.mAttackAnim;
        mOAttackAnim  = observer.mOAttackAnim;
        mSwingTime    = observer.mSwingTime;
        mSwingingHand = observer.mSwingingHand;
        mItemInUse    = observer.mItemInUse.get();

        observer.mAttackAnim   = static_cast<float>(subject.mAttackAnim);
        observer.mOAttackAnim  = static_cast<float>(subject.mOAttackAnim);
        observer.mSwingTime    = static_cast<int>(subject.mSwingTime);
        observer.mSwingingHand = static_cast<std::optional<HandSlot>>(subject.mSwingingHand);
        observer.mItemInUse    = subject.mItemInUse.get();

        auto& context = observer.getEntityContext();
        if (auto current = context.tryGetComponent<ItemInUseComponent>()) mSavedUse = *current;
        if (auto use = subject.getEntityContext().tryGetComponent<ItemInUseComponent>()) {
            context.getOrAddComponent<ItemInUseComponent>() = *use;
        } else if (mSavedUse) {
            context.removeComponent<ItemInUseComponent>();
        }
    }

    ~ScopedHandAction() {
        mObserver.mAttackAnim   = mAttackAnim;
        mObserver.mOAttackAnim  = mOAttackAnim;
        mObserver.mSwingTime    = mSwingTime;
        mObserver.mSwingingHand = mSwingingHand;
        mObserver.mItemInUse    = mItemInUse;

        auto& context = mObserver.getEntityContext();
        if (mSavedUse) context.getOrAddComponent<ItemInUseComponent>() = *mSavedUse;
        else context.removeComponent<ItemInUseComponent>();
    }

    ScopedHandAction(ScopedHandAction const&)            = delete;
    ScopedHandAction& operator=(ScopedHandAction const&) = delete;

private:
    Player&                           mObserver;
    float                             mAttackAnim{};
    float                             mOAttackAnim{};
    int                               mSwingTime{};
    std::optional<HandSlot>           mSwingingHand;
    PlayerItemInUse                   mItemInUse;
    std::optional<ItemInUseComponent> mSavedUse;
};
LL_TYPE_INSTANCE_HOOK(
    HandItemInHandHook,
    ll::memory::HookPriority::High,
    InGamePlayScreen,
    runtime::gameAddress(runtime::GameFunction::RenderItemInHand),
    void,
    ScreenContext& screenContext,
    Player&        player
) {
    auto* subject = player.isLocalPlayer() ? subjectPlayer() : nullptr;
    if (!subject) {
        origin(screenContext, player);
        return;
    }
    if (subjectSleepSuppressesHand()) return;
    ItemStack const savedCarried = player.getCarriedItem();
    ItemStack const savedOffhand = player.getOffhandSlot();
    player.setCarriedItem(subject->getCarriedItem());
    player.setOffhandSlot(subject->getOffhandSlot());
    ScopedHandPass         pass(*subject);
    ScopedObserverGameType gameType(player, *subject);
    ScopedHandAction       action(player, *subject);
    ScopedHandSway         sway(*this, player, *subject);
    origin(screenContext, player);
    player.setCarriedItem(savedCarried);
    player.setOffhandSlot(savedOffhand);
}

LL_TYPE_INSTANCE_HOOK(
    HandRenderFirstPersonHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::renderFirstPerson,
    void,
    BaseActorRenderContext& context,
    Matrix const&           prevProj,
    ItemContextFlags        itemFlags
) {
    auto* subject = subjectPlayer();
    if (!subject) {
        origin(context, prevProj, itemFlags);
        return;
    }
    if (subjectSleepSuppressesHand()) return;
    syncRenderer(*this, *subject);
    ScopedHandPass pass(*subject);
    origin(context, prevProj, itemFlags);
}

// 26.51 no longer exports _renderFirstPersonHandsAndAttachables, _renderItemInMainHand/_renderItemInOffhand,
// _shouldRenderOffhandItem and renderMainhandItem, nor 26.60 renderOffhandItem; their hooks only passed the player
// through unchanged.

LL_TYPE_INSTANCE_HOOK(
    HandMiniMapHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::_renderMiniMapHand,
    void,
    BaseActorRenderContext& context,
    Player&                 player,
    bool                    inOffhand
) {
    origin(context, handPlayer(player), inOffhand);
}

// LevelRendererPlayer::bobView reads its bob from IClientInstance::getCameraEntity, the observer while following:
// WalkDistComponent, PlayerBobComponent and the interpolated Mob::mOTilt / mTilt pair.
// The subject's values stand in for one call so the first-person view and the held map sway with the recorded walk.
class ScopedObserverBob {
public:
    ScopedObserverBob(Mob& observer, Mob& subject) : mObserver(observer) {
        auto& to   = observer.getEntityContext();
        auto& from = subject.getEntityContext();
        mWalk      = to.tryGetComponent<WalkDistComponent>();
        mBob       = to.tryGetComponent<PlayerBobComponent>();
        auto walk  = from.tryGetComponent<WalkDistComponent>();
        auto bob   = from.tryGetComponent<PlayerBobComponent>();
        if (!mWalk || !mBob || !walk || !bob) {
            mWalk = nullptr;
            return;
        }
        mSavedWalk      = *mWalk;
        mSavedBob       = *mBob;
        mSavedOTilt     = observer.mOTilt;
        mSavedTilt      = observer.mTilt;
        *mWalk          = *walk;
        *mBob           = *bob;
        observer.mOTilt = subject.mOTilt;
        observer.mTilt  = subject.mTilt;
    }

    ~ScopedObserverBob() {
        if (!mWalk) return;
        *mWalk           = mSavedWalk;
        *mBob            = mSavedBob;
        mObserver.mOTilt = mSavedOTilt;
        mObserver.mTilt  = mSavedTilt;
    }

    ScopedObserverBob(ScopedObserverBob const&)            = delete;
    ScopedObserverBob& operator=(ScopedObserverBob const&) = delete;

private:
    Mob&                mObserver;
    WalkDistComponent*  mWalk{};
    PlayerBobComponent* mBob{};
    WalkDistComponent   mSavedWalk{};
    PlayerBobComponent  mSavedBob{};
    float               mSavedOTilt{};
    float               mSavedTilt{};
};

LL_TYPE_INSTANCE_HOOK(
    BobViewHook,
    ll::memory::HookPriority::High,
    LevelRendererPlayer,
    &LevelRendererPlayer::bobView,
    void,
    ::Matrix& view,
    float     alpha
) {
    auto* subject = isReplaySubjectViewDriven() ? subjectPlayer() : nullptr;
    auto  client  = subject ? ll::service::getClientInstance() : nullptr;
    auto* local   = client ? client->getLocalPlayer() : nullptr;
    if (!local || static_cast<Actor*>(local) == static_cast<Actor*>(subject)) {
        origin(view, alpha);
        return;
    }
    // The subject's walk distance advances per replay tick; the native alpha only matches the replay at 1x.
    if (auto const context = keyframe::currentCameraTimelineRenderContext(); context && context->time.isValid()) {
        auto const tick = context->time.requiredAppliedTick();
        alpha = std::clamp(static_cast<float>(context->time.value() - static_cast<long double>(tick - 1)), 0.0f, 1.0f);
    }
    ScopedObserverBob swap(*local, *subject);
    origin(view, alpha);
}

uintptr_t functionStart(void* address) {
    DWORD64 imageBase = 0;
    auto*   entry     = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address), &imageBase, nullptr);
    return entry ? static_cast<uintptr_t>(imageBase + entry->BeginAddress) : 0;
}

// On 1.26.51.01 the fire and in-block overlays are inlined into _renderLevelPrep.
// _renderTransparentFirstPerson3DObjects stays on the observer: its portal overlay reads LocalPlayer-only fields
// inline, past the end of a remote player, and it hands its actor to _renderItemInHand, which copies the subject's
// items onto the observer itself.
bool isOverlayCaller(void* returnAddress) {
    static uintptr_t const levelPrep =
        reinterpret_cast<uintptr_t>(ll::memory::resolveIdentifier(&InGamePlayScreen::$_renderLevelPrep));
    static uintptr_t const firstPersonObjects =
        runtime::gameAddress(runtime::GameFunction::ShouldRenderFirstPersonObjects);
    auto const start = functionStart(returnAddress);
    return start != 0 && (start == levelPrep || start == firstPersonObjects);
}

bool isPlayerScoping(Player const& player) {
    static auto const fn = runtime::gameMember<bool (Player::*)() const>(runtime::GameFunction::PlayerIsScoping);
    return (player.*fn)();
}

// Follow mode parks the observer on the subject, so only the overlays are redirected; the render list and name tags
// keep the observer.
LL_TYPE_INSTANCE_HOOK(
    CameraTargetHook,
    ll::memory::HookPriority::High,
    ClientInstance,
    runtime::gameAddress(runtime::GameFunction::GetCameraTargetActor),
    Actor*
) {
    auto* actor = origin();
    if (!actor || !actor->isPlayer() || !static_cast<Player*>(actor)->isLocalPlayer()) return actor;
    auto* subject = subjectPlayer();
    if (!subject || !isOverlayCaller(_ReturnAddress())) return actor;
    // The scope overlay reads LocalPlayer-only members, which a remote subject lacks; it follows the observer via
    // isScoping.
    // 26.51 no longer exports Player::isUsingItem; it reported a non-empty mItemInUse item.
    bool const usingItem = !subject->mItemInUse->mItem->isNull();
    return !isPlayerScoping(*subject) && !usingItem ? subject : actor;
}

// Vanilla darkens a sleeping player's screen with PlayerSleepFadeEffectSystem::Fade, which runs the registry-context
// CameraFadeEffectComponent animation: a full-screen quad of colour (16, 16, 32) / 255 whose alpha rises linearly from
// 0 to 0.863 over 5 s and holds while the player sleeps; PlayerSleepFadeEffectSystem::Wakeup clears it instantly.
// The observer never sleeps, so that component stays idle during a replay. The recorded sleep ticks drive the same
// curve here, drawn as a full-screen fill from the HUD vignette pass (1.26.20.04).
constexpr float SleepFadeSeconds  = 5.0f;
constexpr float SleepFadeTargetA  = 0.863f;
constexpr float SleepFadeTickRate = 20.0f;
constexpr float SleepFadeRed      = 16.0f / 255.0f;
constexpr float SleepFadeGreen    = 16.0f / 255.0f;
constexpr float SleepFadeBlue     = 32.0f / 255.0f;

LL_TYPE_INSTANCE_HOOK(
    SleepVignetteHook,
    ll::memory::HookPriority::High,
    HudVignetteRenderer,
    runtime::gameAddress(runtime::GameFunction::HudVignetteRender),
    void,
    MinecraftUIRenderContext& renderContext,
    IClientInstance&          client,
    UIControl&                owner,
    int                       pass
) {
    // 26.51 no longer exports _renderVignette; the fill uses the vignette control's own size.
    auto const  size              = owner.mSize.get();
    float const w                 = size.x;
    float const h                 = size.y;
    bool const  thirdPersonFollow = isReplaySubjectThirdPerson() && isReplaySubjectFollowed();
    if (isReplaySubjectViewDriven() && !thirdPersonFollow && replaySubjectViewWeight() > 0.0f) {
        float const progress =
            std::clamp(replaySubjectSleepTicks() / (SleepFadeSeconds * SleepFadeTickRate), 0.0f, 1.0f);
        float const alpha = SleepFadeTargetA * progress;
        if (alpha > 0.0f) {
            RectangleArea area{};
            area._x0 = 0.0f;
            area._x1 = w;
            area._y0 = 0.0f;
            area._y1 = h;
            renderContext.fillRectangle(area, mce::Color(SleepFadeRed, SleepFadeGreen, SleepFadeBlue, 1.0f), alpha);
        }
    }
    origin(renderContext, client, owner, pass);
}

// The recorded perspective is first person; a toggle while following would be undone by the follow loop anyway.
LL_TYPE_STATIC_HOOK(
    TogglePerspectiveHook,
    ll::memory::HookPriority::High,
    ClientInputCallbacks,
    &ClientInputCallbacks::handleTogglePerspectiveButtonPress,
    void,
    IClientInstance& client
) {
    if (isReplaySubjectFollowed()) return;
    origin(client);
}

LL_TYPE_INSTANCE_HOOK(
    PlayerIsScopingHook,
    ll::memory::HookPriority::High,
    Player,
    runtime::gameAddress(runtime::GameFunction::PlayerIsScoping),
    bool
) {
    bool const scoping = origin();
    if (!isLocalPlayer()) return scoping;
    auto* subject = subjectPlayer();
    return scoping || !subject || subject == this ? scoping : isPlayerScoping(*subject);
}

// 26.51 inlines the portal and in-block overlays into $_renderTransparentFirstPerson3DObjects and reads the observer.
// A remote subject is shorter than those LocalPlayer fields, so the actor pointer stays the observer and the two
// values it tests are written for the call only.

// Vanilla grows the distortion 0.0066667 per tick while nausea lasts over 60 ticks and fades it 0.05 per tick.
struct NauseaRamp {
    bool        valid{};
    bool        active{};
    long double tick{};
    float       level{};
};

// A nether portal adds 0.0125 per tick and fades 0.05. It is not the nausea effect.
struct PortalRamp {
    bool        valid{};
    long double tick{};
    float       level{};
};

NauseaRamp       gNausea;
PortalRamp       gPortal;
std::atomic_bool gNauseaDistorting{false};

float advanceNausea(SubjectNauseaSample const& sample) {
    auto& ramp = gNausea;
    if (ramp.valid) {
        float const ticks = static_cast<float>(std::clamp<long double>(sample.tick - ramp.tick, 0.0L, 8.0L));
        ramp.level        = std::clamp(ramp.level + (sample.active ? 0.0066667f : -0.05f) * ticks, 0.0f, 1.0f);
    }
    ramp.valid  = true;
    ramp.tick   = sample.tick;
    ramp.active = sample.active;
    return ramp.level;
}

float advancePortal(SubjectNauseaSample const& sample) {
    auto& ramp = gPortal;
    // A seek backwards must not keep a warp that belongs to a later tick.
    if (!ramp.valid || sample.tick < ramp.tick) ramp.level = 0.0f;
    else {
        float const ticks = static_cast<float>(std::clamp<long double>(sample.tick - ramp.tick, 0.0L, 8.0L));
        ramp.level        = std::clamp(ramp.level + (sample.insidePortal ? 0.0125f : -0.05f) * ticks, 0.0f, 1.0f);
    }
    ramp.valid = true;
    ramp.tick  = sample.tick;
    return ramp.level;
}

void resetWarp() {
    gNausea = NauseaRamp{};
    gPortal = PortalRamp{};
    gNauseaDistorting.store(false, std::memory_order_release);
}

// The distortion reads the observer's own effects, which have none; follow mode feeds it the subject's nausea.
// A portal uses the same data at 20 per tick; nausea, when it is warping, stays at 7 and hides the purple screen.
LL_TYPE_INSTANCE_HOOK(
    PortalDataHook,
    ll::memory::HookPriority::High,
    VanillaCameraAPI,
    runtime::gameAddress(runtime::GameFunction::TryGetPortalData),
    std::optional<IVanillaCameraAPI::PortalData>
) {
    auto       data   = origin();
    auto const sample = sampleReplaySubjectNausea();
    if (!sample) {
        resetWarp();
        return data;
    }
    float const nausea = advanceNausea(*sample);
    float const portal = sample->present ? 0.0f : advancePortal(*sample);
    float const level  = std::max(nausea, portal);
    gNauseaDistorting.store(level > 0.0f && data.has_value(), std::memory_order_release);
    if (level <= 0.0f || !data) return data;
    data->mTime = std::max(data->mTime, level);
    // The game spins by speed * (frame alpha + client tick), a clock that keeps running while the replay is paused.
    // Confusion keeps the slow wobble for the whole effect, including its fade. A portal alone spins faster.
    float const speed = sample->present ? 7.0f : 20.0f;
    data->mRotation   = static_cast<float>(static_cast<long double>(speed) * sample->tick);
    return data;
}

// Writes the subject's portal time onto the observer and clears NoClip for one vanilla overlay call, then restores
// both.
struct ScopedObserverOverlay {
    LocalPlayer*        player{};
    float               portal{};
    float               oldPortal{};
    bool                abilities{};
    std::array<bool, 6> noclip{};

    ScopedObserverOverlay(LocalPlayer& local, float level) : player(&local) {
        portal                   = local.mPortalEffectTime;
        oldPortal                = local.mOPortalEffectTime;
        local.mPortalEffectTime  = level;
        local.mOPortalEffectTime = level;
        if (auto* component = local.getEntityContext().tryGetComponent<AbilitiesComponent>().as_ptr()) {
            abilities    = true;
            auto& layers = component->mAbilities->mLayers.get();
            for (size_t i = 0; i < layers.size() && i < noclip.size(); ++i) {
                auto& ability            = layers[i].mAbilities.get()[static_cast<size_t>(AbilitiesIndex::NoClip)];
                noclip[i]                = ability.mValue->mBoolVal;
                ability.mValue->mBoolVal = false;
            }
        }
    }

    ~ScopedObserverOverlay() {
        if (!player) return;
        player->mPortalEffectTime  = portal;
        player->mOPortalEffectTime = oldPortal;
        if (!abilities) return;
        if (auto* component = player->getEntityContext().tryGetComponent<AbilitiesComponent>().as_ptr()) {
            auto& layers = component->mAbilities->mLayers.get();
            for (size_t i = 0; i < layers.size() && i < noclip.size(); ++i) {
                layers[i].mAbilities.get()[static_cast<size_t>(AbilitiesIndex::NoClip)].mValue->mBoolVal = noclip[i];
            }
        }
    }
};

thread_local bool   gOverlaySample = false;
thread_local Actor* gOverlayLocal  = nullptr;

// The observer's own render offset sits on a block face and the overlay strobes, so the pass samples the subject.
LL_TYPE_STATIC_HOOK(
    OverlayEyeSampleHook,
    ll::memory::HookPriority::High,
    VanillaOffsetSystem,
    runtime::gameAddress(runtime::GameFunction::GetCameraPosition),
    Vec3,
    Actor const& actor,
    float        alpha
) {
    if (gOverlaySample && &actor == gOverlayLocal) {
        if (auto* subject = subjectPlayer()) return origin(*subject, alpha);
    }
    return origin(actor, alpha);
}
struct ScopedOverlaySample {
    explicit ScopedOverlaySample(Actor* local) {
        gOverlayLocal  = local;
        gOverlaySample = true;
    }

    ~ScopedOverlaySample() {
        gOverlaySample = false;
        gOverlayLocal  = nullptr;
    }
};

// The flame quads call Actor::isOnFire on the camera actor. The observer is never burning; the subject is.
LL_TYPE_INSTANCE_HOOK(
    OverlayOnFireHook,
    ll::memory::HookPriority::High,
    Actor,
    runtime::gameAddress(runtime::GameFunction::ActorIsOnFire),
    bool
) {
    if (gOverlaySample && this == gOverlayLocal) {
        if (auto* subject = subjectPlayer(); subject && static_cast<Actor*>(subject) != this)
            return subject->isOnFire();
    }
    return origin();
}

// The purple warp and the buried-block texture both draw here. Redirecting its actor reads past a remote player.
LL_TYPE_INSTANCE_HOOK(
    TransparentOverlayHook,
    ll::memory::HookPriority::High,
    InGamePlayScreen,
    runtime::gameAddress(runtime::GameFunction::RenderTransparentFirstPerson3DObjects),
    void,
    ScreenContext& screenContext,
    LevelRenderer& levelRenderer
) {
    bool const firstPerson = (isReplaySubjectFollowed() || isReplaySubjectHiddenFor(SubjectHideReason::Export))
                          && !isReplaySubjectThirdPerson();
    auto* subject = firstPerson ? replayHiddenActorInScope() : nullptr;
    if (!subject) {
        origin(screenContext, levelRenderer);
        return;
    }
    auto const sample = sampleReplaySubjectNausea();
    if (!sample) {
        resetWarp();
        origin(screenContext, levelRenderer);
        return;
    }
    float const portal = sample->present ? 0.0f : advancePortal(*sample);
    auto        client = ll::service::getClientInstance();
    auto*       local  = client ? client->getLocalPlayer() : nullptr;
    if (!local) {
        origin(screenContext, levelRenderer);
        return;
    }
    ScopedObserverOverlay overlay(*local, portal);
    ScopedOverlaySample   sampleScope(local);
    origin(screenContext, levelRenderer);
}

struct HookSlot {
    char const* name;
    bool (*hook)();
    void (*unhook)();
    bool ok;
};

template <class H>
HookSlot makeSlot(char const* name) {
    return HookSlot{name, [] { return H::hook() == 0; }, [] { H::unhook(); }, false};
}

} // namespace

bool isReplayFirstPersonHandPass() { return gPassDepth > 0; }

bool isReplayNauseaDistortionActive() { return gNauseaDistorting.load(std::memory_order_acquire); }

void hookReplayFirstPersonHands(bool enable, SubjectHideReason owner) {
    static bool                  installed = false;
    static std::vector<HookSlot> slots;
    static uint8_t               owners = 0;

    auto const bit = static_cast<uint8_t>(owner);
    if (enable) owners |= bit;
    else owners &= static_cast<uint8_t>(~bit);

    if (enable) {
        if (installed) return;
        gHeights.reset();
        slots = {
            makeSlot<HandGameTypeHook>("gameType"),
            makeSlot<HandItemInHandHook>("itemInHand"),
            makeSlot<HandRenderFirstPersonHook>("renderFirstPerson"),
            makeSlot<HandMiniMapHook>("miniMap"),
            makeSlot<CameraTargetHook>("cameraTarget"),
            makeSlot<BobViewHook>("bobView"),
            makeSlot<PortalDataHook>("portalData"),
            makeSlot<TransparentOverlayHook>("transparentOverlay"),
            makeSlot<OverlayEyeSampleHook>("eyeSample"),
            makeSlot<OverlayOnFireHook>("onFire"),
            makeSlot<PlayerIsScopingHook>("isScoping"),
            makeSlot<SleepVignetteHook>("sleepVignette"),
            makeSlot<TogglePerspectiveHook>("togglePerspective"),
        };
        bool       complete = true;
        auto const begin    = std::chrono::steady_clock::now();
        for (auto& slot : slots) {
            slot.ok   = slot.hook();
            complete &= slot.ok;
        }
        installed = true;
        Playback::getInstance().getSelf().getLogger().debug(
            "Replay first-person hand hooks installed in {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count()
        );
        if (!complete) {
            std::string missing;
            for (auto const& slot : slots) {
                if (!slot.ok) missing += std::string(" ") + slot.name;
            }
            Playback::getInstance().getSelf().getLogger().warn("Replay first-person hand hooks incomplete:{}", missing);
        }
        return;
    }

    if (!installed || owners != 0) return;
    for (auto& slot : slots) {
        if (slot.ok) slot.unhook();
    }
    slots.clear();
    gHeights.reset();
    installed = false;
}

} // namespace playback::visuals
