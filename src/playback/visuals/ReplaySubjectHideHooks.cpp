#include "ReplaySubjectHideHooks.h"

#include "ReplayEntityInterpolator.h"
#include "ReplayFirstPersonHandHooks.h"
#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"

#include <chrono>

namespace playback::visuals {

namespace {

using RenderBasic  = void (ActorRenderDispatcher::*)(BaseActorRenderContext&, Actor&, bool);
using RenderPosRot = void (ActorRenderDispatcher::*)(BaseActorRenderContext&, Actor&, Vec3 const&, Vec2 const&, bool);
using RenderCamera =
    void (ActorRenderDispatcher::*)(BaseActorRenderContext&, Actor&, Vec3 const&, Vec3 const&, Vec2 const&, bool);

LL_TYPE_INSTANCE_HOOK(
    SubjectHideRenderBasicHook,
    ll::memory::HookPriority::Lowest,
    ActorRenderDispatcher,
    static_cast<RenderBasic>(&ActorRenderDispatcher::render),
    void,
    BaseActorRenderContext& context,
    Actor&                  entity,
    bool                    ignoreLighting
) {
    if (isReplayEntityHiddenInScope(entity) && !isReplayFirstPersonHandPass()) return;
    origin(context, entity, ignoreLighting);
}

LL_TYPE_INSTANCE_HOOK(
    SubjectHideRenderPosRotHook,
    ll::memory::HookPriority::Lowest,
    ActorRenderDispatcher,
    static_cast<RenderPosRot>(&ActorRenderDispatcher::render),
    void,
    BaseActorRenderContext& context,
    Actor&                  entity,
    Vec3 const&             pos,
    Vec2 const&             rot,
    bool                    ignoreLighting
) {
    if (isReplayEntityHiddenInScope(entity) && !isReplayFirstPersonHandPass()) return;
    origin(context, entity, pos, rot, ignoreLighting);
}

LL_TYPE_INSTANCE_HOOK(
    SubjectHideRenderCameraHook,
    ll::memory::HookPriority::Lowest,
    ActorRenderDispatcher,
    static_cast<RenderCamera>(&ActorRenderDispatcher::render),
    void,
    BaseActorRenderContext& context,
    Actor&                  entity,
    Vec3 const&             cameraTargetPos,
    Vec3 const&             pos,
    Vec2 const&             rot,
    bool                    ignoreLighting
) {
    if (isReplayEntityHiddenInScope(entity) && !isReplayFirstPersonHandPass()) return;
    origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
}

} // namespace

void hookReplaySubjectHide(bool enable, SubjectHideReason owner) {
    static bool    installed = false;
    static bool    basic{}, posRot{}, camera{};
    static uint8_t owners = 0;

    auto const bit = static_cast<uint8_t>(owner);
    if (enable) owners |= bit;
    else owners &= static_cast<uint8_t>(~bit);

    if (enable) {
        if (installed) return;
        auto const begin = std::chrono::steady_clock::now();
        basic            = SubjectHideRenderBasicHook::hook() == 0;
        posRot           = SubjectHideRenderPosRotHook::hook() == 0;
        camera           = SubjectHideRenderCameraHook::hook() == 0;
        installed        = true;
        Playback::getInstance().getSelf().getLogger().debug(
            "Replay subject hide hooks installed in {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count()
        );
        if (!basic || !posRot || !camera) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Replay subject hide hooks incomplete (basic={}, posRot={}, camera={})",
                basic,
                posRot,
                camera
            );
        }
        return;
    }

    if (!installed || owners != 0) return;
    if (basic) SubjectHideRenderBasicHook::unhook();
    if (posRot) SubjectHideRenderPosRotHook::unhook();
    if (camera) SubjectHideRenderCameraHook::unhook();
    installed = false;
}

} // namespace playback::visuals
