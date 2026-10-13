#include "ReplaySubjectHideHooks.h"

#include "ReplayEntityInterpolator.h"
#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"

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
    if (isReplayEntityHiddenInScope(entity)) return;
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
    if (isReplayEntityHiddenInScope(entity)) return;
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
    if (isReplayEntityHiddenInScope(entity)) return;
    origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
}

} // namespace

void hookReplaySubjectHide(bool enable) {
    static bool installed = false;
    static bool basic{}, posRot{}, camera{};

    if (enable) {
        if (installed) return;
        basic     = SubjectHideRenderBasicHook::hook() == 0;
        posRot    = SubjectHideRenderPosRotHook::hook() == 0;
        camera    = SubjectHideRenderCameraHook::hook() == 0;
        installed = true;
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

    if (!installed) return;
    if (basic) SubjectHideRenderBasicHook::unhook();
    if (posRot) SubjectHideRenderPosRotHook::unhook();
    if (camera) SubjectHideRenderCameraHook::unhook();
    installed = false;
}

} // namespace playback::visuals
