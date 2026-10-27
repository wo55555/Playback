#include "ReplayFirstPersonHandHooks.h"

#include "ReplayEntityInterpolator.h"
#include "ReplayHandArm.h"
#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineRegistry.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "mc-external/IOptionRegistry.h"
#include "mc/client/game/ClientInputCallbacks.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/controls/renderers/HudVignetteRenderer.h"
#include "mc/client/gui/screens/InGamePlayScreen.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/vanilla_components/PlayerBobComponent.h"
#include "mc/entity/components/WalkDistComponent.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/vanilla_camera/VanillaCameraAPI.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
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
    renderer.mItem             = state.main.item;
    renderer.mOffHandItem      = state.off.item;
    renderer.mSlot             = state.main.slot;
    renderer.mHeight           = mainHeight;
    renderer.mOldHeight        = mainHeight;
    renderer.mHeightOffHand    = offHeight;
    renderer.mOldHeightOffHand = offHeight;
}

// The game's view-bobbing setting; the hand bob follows it like the native first-person view does.
bool viewBobbingEnabled() {
    auto client = ll::service::getClientInstance();
    return client && client->getOptions().getBobView();
}

// The scope hides the subject with Invisible flags; its hands must render without them.
class ScopedHandPass {
public:
    explicit ScopedHandPass(Player& subject)
    : mSubject(subject),
      mOuter(gPassDepth == 0),
      mInvisible(subject.getStatusFlag(ActorFlags::Invisible)),
      mHiddenWhenInvisible(subject.getStatusFlag(ActorFlags::HiddenWhenInvisible)) {
        if (mOuter) {
            mSubject.setStatusFlag(ActorFlags::Invisible, false);
            mSubject.setStatusFlag(ActorFlags::HiddenWhenInvisible, false);

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
            mSubject.setStatusFlag(ActorFlags::HiddenWhenInvisible, mHiddenWhenInvisible);
            mSubject.setStatusFlag(ActorFlags::Invisible, mInvisible);
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

LL_TYPE_INSTANCE_HOOK(
    HandShowHook,
    ll::memory::HookPriority::High,
    InGamePlayScreen,
    &InGamePlayScreen::_showHand,
    bool
) {
    auto const shown = origin();
    return shown || subjectPlayer() != nullptr;
}

LL_TYPE_INSTANCE_HOOK(
    HandItemInHandHook,
    ll::memory::HookPriority::High,
    InGamePlayScreen,
    &InGamePlayScreen::$_renderItemInHand,
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
    ScopedHandPass pass(*subject);
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
    ItemContextFlags        itemFlags
) {
    auto* subject = subjectPlayer();
    if (!subject) {
        origin(context, itemFlags);
        return;
    }
    if (subjectSleepSuppressesHand()) return;
    syncRenderer(*this, *subject);
    ScopedHandPass pass(*subject);
    origin(context, itemFlags);
}

LL_TYPE_INSTANCE_HOOK(
    HandHandsHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::_renderFirstPersonHandsAndAttachables,
    void,
    BaseActorRenderContext& context,
    Player&                 player
) {
    origin(context, handPlayer(player));
}

LL_TYPE_INSTANCE_HOOK(
    HandMainHandHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::_renderItemInMainHand,
    void,
    BaseActorRenderContext& context,
    Player&                 player,
    float                   inverseArmHeight,
    float                   attackValue,
    float                   unknown
) {
    origin(context, handPlayer(player), inverseArmHeight, attackValue, unknown);
}

LL_TYPE_INSTANCE_HOOK(
    HandOffhandHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::_renderItemInOffhand,
    void,
    BaseActorRenderContext& context,
    Player&                 player,
    float                   a,
    float                   b,
    float                   c
) {
    origin(context, handPlayer(player), a, b, c);
}

LL_TYPE_INSTANCE_HOOK(
    HandShouldRenderOffhandHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::_shouldRenderOffhandItem,
    bool,
    Player& player
) {
    return origin(handPlayer(player));
}

LL_TYPE_INSTANCE_HOOK(
    HandMainhandItemHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::renderMainhandItem,
    void,
    BaseActorRenderContext& context,
    Player&                 player,
    ItemContextFlags        itemFlags
) {
    origin(context, handPlayer(player), itemFlags);
}

LL_TYPE_INSTANCE_HOOK(
    HandOffhandItemHook,
    ll::memory::HookPriority::High,
    ItemInHandRenderer,
    &ItemInHandRenderer::renderOffhandItem,
    void,
    BaseActorRenderContext& context,
    Player&                 player,
    ItemContextFlags        itemFlags
) {
    origin(context, handPlayer(player), itemFlags);
}

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

// LevelRendererPlayer::bobView reads its bob from IClientInstance::getCameraEntity, the observer while following
// (1.26.20.04): WalkDistComponent, PlayerBobComponent and an interpolated Actor float pair at +0x3B4/+0x3B8 (tilt).
// The subject's values stand in for one call so the first-person view and the held map sway with the recorded walk.
constexpr size_t BobTiltOffset = 0x3B4;

class ScopedObserverBob {
public:
    ScopedObserverBob(Actor& observer, Actor& subject) {
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
        mTilt      = reinterpret_cast<float*>(reinterpret_cast<std::byte*>(&observer) + BobTiltOffset);
        mSavedWalk = *mWalk;
        mSavedBob  = *mBob;
        std::memcpy(mSavedTilt, mTilt, sizeof(mSavedTilt));
        *mWalk = *walk;
        *mBob  = *bob;
        std::memcpy(mTilt, reinterpret_cast<std::byte const*>(&subject) + BobTiltOffset, sizeof(mSavedTilt));
    }

    ~ScopedObserverBob() {
        if (!mWalk) return;
        *mWalk = mSavedWalk;
        *mBob  = mSavedBob;
        std::memcpy(mTilt, mSavedTilt, sizeof(mSavedTilt));
    }

    ScopedObserverBob(ScopedObserverBob const&)            = delete;
    ScopedObserverBob& operator=(ScopedObserverBob const&) = delete;

private:
    WalkDistComponent*  mWalk{};
    PlayerBobComponent* mBob{};
    float*              mTilt{};
    WalkDistComponent   mSavedWalk{};
    PlayerBobComponent  mSavedBob{};
    float               mSavedTilt[2]{};
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

// Client RVA span of the screen-overlay renderers (fire, in-block); every other caller keeps the observer.
constexpr uintptr_t OverlayCallerBegin = 0x1DCC5B0;
constexpr uintptr_t OverlayCallerEnd   = 0x1DD1D82;

bool isOverlayCaller(void* returnAddress) {
    static auto const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto const        rva  = reinterpret_cast<uintptr_t>(returnAddress) - base;
    return rva >= OverlayCallerBegin && rva < OverlayCallerEnd;
}

// Follow mode parks the observer on the subject, so only the overlays are redirected; the render list and name tags
// keep the observer.
LL_TYPE_INSTANCE_HOOK(
    CameraTargetHook,
    ll::memory::HookPriority::High,
    ClientInstance,
    &ClientInstance::$getCameraTargetActor,
    Actor*
) {
    auto* actor = origin();
    if (!actor || !actor->isPlayer() || !static_cast<Player*>(actor)->isLocalPlayer()) return actor;
    auto* subject = subjectPlayer();
    if (!subject || !isOverlayCaller(_ReturnAddress())) return actor;
    // The scope overlay reads LocalPlayer-only members, which a remote subject lacks; it follows the observer via
    // isScoping.
    return !subject->isScoping() && !subject->isUsingItem() ? subject : actor;
}

// Vanilla darkens a sleeping player's screen with PlayerSleepFadeEffectSystem::Fade, which runs the registry-context
// CameraFadeEffectComponent animation: a full-screen quad of colour (16, 16, 32) / 255 whose alpha rises linearly from
// 0 to 0.863 over 5 s and holds while the player sleeps; PlayerSleepFadeEffectSystem::Wakeup clears it instantly.
// The observer never sleeps, so that component stays idle during a replay. The recorded sleep ticks drive the same
// curve here, drawn as a full-screen fill from the HUD vignette pass (1.26.20.04).
constexpr float SleepFadeSeconds   = 5.0f;
constexpr float SleepFadeTargetA   = 0.863f;
constexpr float SleepFadeTickRate  = 20.0f;
constexpr float SleepFadeRed       = 16.0f / 255.0f;
constexpr float SleepFadeGreen     = 16.0f / 255.0f;
constexpr float SleepFadeBlue      = 32.0f / 255.0f;

LL_TYPE_INSTANCE_HOOK(
    SleepVignetteHook,
    ll::memory::HookPriority::High,
    HudVignetteRenderer,
    &HudVignetteRenderer::_renderVignette,
    bool,
    MinecraftUIRenderContext& renderContext,
    IClientInstance&          client,
    float                     undergroundness,
    int                       w,
    int                       h
) {
    if (isReplaySubjectViewDriven() && replaySubjectViewWeight() > 0.0f) {
        float const progress = std::clamp(replaySubjectSleepTicks() / (SleepFadeSeconds * SleepFadeTickRate), 0.0f, 1.0f);
        float const alpha    = SleepFadeTargetA * progress;
        if (alpha > 0.0f) {
            renderContext.fillRectangle(
                RectangleArea(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), false),
                mce::Color(SleepFadeRed, SleepFadeGreen, SleepFadeBlue, 1.0f),
                alpha
            );
        }
    }
    return origin(renderContext, client, undergroundness, w, h);
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

LL_TYPE_INSTANCE_HOOK(PlayerIsScopingHook, ll::memory::HookPriority::High, Player, &Player::isScoping, bool) {
    bool const scoping = origin();
    if (!isLocalPlayer()) return scoping;
    auto* subject = subjectPlayer();
    return scoping || !subject || subject == this ? scoping : subject->isScoping();
}

// The screen overlay reads these LocalPlayer-only fields from the remote subject, whose smaller object yields heap
// garbage.
float neutralPortalTime(LocalPlayer const& self, float raw) {
    return static_cast<Player const&>(self).isLocalPlayer() ? raw : 0.0f;
}

LL_TYPE_INSTANCE_HOOK(
    PortalEffectTimeHook,
    ll::memory::HookPriority::High,
    LocalPlayer,
    &LocalPlayer::getPortalEffectTime,
    float
) {
    return neutralPortalTime(*this, origin());
}

LL_TYPE_INSTANCE_HOOK(
    OPortalEffectTimeHook,
    ll::memory::HookPriority::High,
    LocalPlayer,
    &LocalPlayer::getOPortalEffectTime,
    float
) {
    return neutralPortalTime(*this, origin());
}

// Vanilla grows the distortion 0.0066667 per tick while nausea lasts over 60 ticks and fades it 0.05 per tick.
struct NauseaRamp {
    bool        valid{};
    bool        active{};
    long double tick{};
    float       level{};
};

NauseaRamp       gNausea;
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

// The distortion reads the observer's own effects, which have none; follow mode feeds it the subject's nausea.
LL_TYPE_INSTANCE_HOOK(
    PortalDataHook,
    ll::memory::HookPriority::High,
    VanillaCameraAPI,
    &VanillaCameraAPI::$tryGetPortalData,
    std::optional<IVanillaCameraAPI::PortalData>
) {
    auto       data   = origin();
    auto const sample = sampleReplaySubjectNausea();
    if (!sample) {
        gNausea = NauseaRamp{};
        gNauseaDistorting.store(false, std::memory_order_release);
        return data;
    }
    float const level = advanceNausea(*sample);
    gNauseaDistorting.store(level > 0.0f && data.has_value(), std::memory_order_release);
    if (level <= 0.0f || !data) return data;
    data->mTime = std::max(data->mTime, level);
    // The game spins by speed * (frame alpha + client tick), a clock that keeps running while the replay is paused.
    // Nausea spins at 7 per tick; drive it from the replay time so a pause freezes it and a seek rewinds it.
    data->mRotation = static_cast<float>(7.0L * sample->tick);
    return data;
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
            makeSlot<HandShowHook>("showHand"),
            makeSlot<HandGameTypeHook>("gameType"),
            makeSlot<HandItemInHandHook>("itemInHand"),
            makeSlot<HandRenderFirstPersonHook>("renderFirstPerson"),
            makeSlot<HandHandsHook>("hands"),
            makeSlot<HandMainHandHook>("mainHand"),
            makeSlot<HandOffhandHook>("offhand"),
            makeSlot<HandShouldRenderOffhandHook>("shouldRenderOffhand"),
            makeSlot<HandMainhandItemHook>("mainhandItem"),
            makeSlot<HandOffhandItemHook>("offhandItem"),
            makeSlot<HandMiniMapHook>("miniMap"),
            makeSlot<CameraTargetHook>("cameraTarget"),
            makeSlot<BobViewHook>("bobView"),
            makeSlot<PortalEffectTimeHook>("portalEffectTime"),
            makeSlot<OPortalEffectTimeHook>("oPortalEffectTime"),
            makeSlot<PortalDataHook>("portalData"),
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
