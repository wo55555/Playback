#include "ReplayFirstPersonHandHooks.h"

#include "ReplayEntityInterpolator.h"
#include "ReplayHandArm.h"
#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineRegistry.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "mc-external/IOptionRegistry.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/InGamePlayScreen.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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

// Only the observer is replaced; any other player passed in is left alone.
Player& handPlayer(Player& player) {
    if (!player.isLocalPlayer()) return player;
    auto* subject = subjectPlayer();
    return subject ? *subject : player;
}

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
    ScopedHandPass pass(*subject);
    origin(screenContext, *subject);
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

void hookReplayFirstPersonHands(bool enable) {
    static bool                  installed = false;
    static std::vector<HookSlot> slots;

    if (enable) {
        if (installed) return;
        gHeights.reset();
        slots = {
            makeSlot<HandShowHook>("showHand"),
            makeSlot<HandItemInHandHook>("itemInHand"),
            makeSlot<HandRenderFirstPersonHook>("renderFirstPerson"),
            makeSlot<HandHandsHook>("hands"),
            makeSlot<HandMainHandHook>("mainHand"),
            makeSlot<HandOffhandHook>("offhand"),
            makeSlot<HandShouldRenderOffhandHook>("shouldRenderOffhand"),
            makeSlot<HandMainhandItemHook>("mainhandItem"),
            makeSlot<HandOffhandItemHook>("offhandItem"),
            makeSlot<HandMiniMapHook>("miniMap"),
        };
        bool complete = true;
        for (auto& slot : slots) {
            slot.ok   = slot.hook();
            complete &= slot.ok;
        }
        installed = true;
        if (!complete) {
            std::string missing;
            for (auto const& slot : slots) {
                if (!slot.ok) missing += std::string(" ") + slot.name;
            }
            Playback::getInstance().getSelf().getLogger().warn("Replay first-person hand hooks incomplete:{}", missing);
        }
        return;
    }

    if (!installed) return;
    for (auto& slot : slots) {
        if (slot.ok) slot.unhook();
    }
    slots.clear();
    gHeights.reset();
    installed = false;
}

} // namespace playback::visuals
