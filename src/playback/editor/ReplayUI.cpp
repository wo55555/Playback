#include "ReplayUI.h"

#include "playback/editor/renderer/D3D12Hooks.h"
#include "playback/editor/renderer/ImGuiRenderer.h"
#include "playback/editor/renderer/ReplayMouseHook.h"

#include "playback/Playback.h"
#include "playback/editor/context/EditorContext.h"
#include "playback/editor/controller/EditorController.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/functions/record/Recorder.h"

#include <utility>

namespace playback::editor {

namespace {
EditorContext    gContext;
EditorController gController{gContext};
} // namespace

bool hookReplayUIRendererInit(bool enable) { return renderer::hookRendererInit(enable); }

bool hookReplayUI(bool enable) {
    if (enable) {
        gController.reset();
        gContext.reset();
        renderer::gImGuiRenderer.setContext(&gContext);
        functions::Recorder::getInstance().setThumbnailCaptureProvider(&renderer::gImGuiRenderer);
        renderer::setReplayUIActive(true);

        ui::ReplayEditor::getInstance().initialize();

        // Install early because the renderer-init callback is not guaranteed during enable().
        if (!hookReplayUIRendererInit(true)) {
            renderer::setReplayUIActive(false);
            functions::Recorder::getInstance().setThumbnailCaptureProvider(nullptr);
            renderer::gImGuiRenderer.setContext(nullptr);
            ui::ReplayEditor::getInstance().shutdown();
            gContext.reset();
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
    renderer::setReplayMouseInputActive(false);
    functions::Recorder::getInstance().setThumbnailCaptureProvider(nullptr);

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

    if (!ok) return false;

    ui::ReplayEditor::getInstance().shutdown();
    renderer::gImGuiRenderer.setContext(nullptr);
    gController.reset();
    gContext.reset();
    return ok;
}

void tickReplayUI(bool hudVisible) { gController.tick(hudVisible); }

void submitEditorAction(EditorAction action) { gContext.submit(std::move(action)); }

} // namespace playback::editor
