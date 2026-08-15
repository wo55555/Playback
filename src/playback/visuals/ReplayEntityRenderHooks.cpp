#include "ReplayEntityRenderHooks.h"

#include "ReplayEntityInterpolator.h"

#include "ll/api/memory/Hook.h"

#include "mc/deps/ecs/strict/StrictEntityContext.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components/RenderPositionComponent.h"
#include "mc/entity/systems/UpdateRenderPosSystem.h"

#include <atomic>

namespace playback::visuals {

namespace {

LL_TYPE_STATIC_HOOK(
    ReplayEntityRenderPositionHook,
    ll::memory::HookPriority::Lowest,
    UpdateRenderPosSystem,
    &UpdateRenderPosSystem::_doUpdateRenderPosSystem,
    void,
    StrictEntityContext const&  context,
    StateVectorComponent const& stateVector,
    RenderPositionComponent&    renderPosition
) {
    origin(context, stateVector, renderPosition);
    reapplyReplayEntityPosition(renderPosition);
}

std::atomic_bool gInstalled{false};

} // namespace

bool hookReplayEntityRender(bool enable) {
    static bool installed = false;
    if (enable) {
        if (!installed) installed = ReplayEntityRenderPositionHook::hook() == 0;
        gInstalled.store(installed, std::memory_order_release);
        return installed;
    }

    if (installed && ReplayEntityRenderPositionHook::unhook()) installed = false;
    gInstalled.store(installed, std::memory_order_release);
    return !installed;
}

bool isReplayEntityRenderInstalled() { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::visuals
