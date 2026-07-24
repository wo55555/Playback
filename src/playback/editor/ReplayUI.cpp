#include "ReplayUI.h"

#include "playback/editor/renderer/D3D12Hooks.h"
#include "playback/editor/renderer/ImGuiRenderer.h"
#include "playback/editor/renderer/ReplayMouseHook.h"

#include "playback/Playback.h"
#include "playback/editor/context/EditorContext.h"
#include "playback/editor/controller/EditorController.h"

namespace playback::editor {

namespace {
EditorContext    gContext;
EditorController gController{gContext};
} // namespace

bool hookReplayUIRendererInit(bool enable) { return renderer::hookRendererInit(enable); }

bool hookReplayUI(bool enable) {
    if (enable) {
        gContext.reset();
        renderer::gImGuiRenderer.setContext(&gContext);
        renderer::setReplayUIActive(true);

        if (!hookReplayUIRendererInit(true)) {
            renderer::setReplayUIActive(false);
            renderer::gImGuiRenderer.setContext(nullptr);
            Playback::getInstance().getSelf().getLogger().error(
                "Unable to install the early D3D12 renderer hook; the replay timeline may be unavailable"
            );
            return false;
        }
        if (!renderer::hookD3D12(true)) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Replay ImGui timeline is unavailable; replay support will continue without it"
            );
        }
        if (!renderer::hookReplayMouse(true)) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Replay mouse hook unavailable; timeline may not be interactive"
            );
        }
        return true;
    }

    renderer::setReplayUIActive(false);

    bool ok = true;
    if (!hookReplayUIRendererInit(false)) {
        Playback::getInstance().getSelf().getLogger().error("Unable to remove the early D3D12 renderer hook");
        ok = false;
    }
    if (!renderer::hookD3D12(false)) {
        Playback::getInstance().getSelf().getLogger().error("Unable to remove replay ImGui timeline hooks");
        ok = false;
    }
    if (!renderer::hookReplayMouse(false)) {
        Playback::getInstance().getSelf().getLogger().error("Unable to remove replay mouse hook");
        ok = false;
    }

    if (ok) {
        renderer::gImGuiRenderer.setContext(nullptr);
        gContext.reset();
    }
    return ok;
}

void tickReplayUI(bool hudVisible) { gController.tick(hudVisible); }

} // namespace playback::editor
