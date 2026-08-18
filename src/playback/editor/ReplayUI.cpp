#include "ReplayUI.h"

#include "playback/editor/graphics/D3D12Hooks.h"
#include "playback/editor/graphics/ImGuiRenderer.h"
#include "playback/editor/graphics/ReplayMouseHook.h"

#include "playback/Playback.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/record/Recorder.h"
#include "playback/replay/ReplaySession.h"
#include "playback/state/EditorContext.h"
#include "playback/state/EditorController.h"


#include <utility>

namespace playback::editor {
using namespace playback::state;

namespace {
EditorContext    gContext;
EditorController gController{gContext};
} // namespace

bool hookReplayUIRendererInit(bool enable) { return editor::graphics::hookRendererInit(enable); }

bool hookReplayUI(bool enable) {
    if (enable) {
        gController.reset();
        gContext.reset();
        editor::graphics::gImGuiRenderer.setContext(&gContext);
        gController.setFrameTap(&editor::graphics::gImGuiRenderer.frameTap());
        record::Recorder::getInstance().setThumbnailCaptureProvider(&editor::graphics::gImGuiRenderer);
        editor::graphics::setReplayUIActive(true);

        ui::ReplayEditor::getInstance().initialize();

        // Install early because the renderer-init callback is not guaranteed during enable().
        if (!hookReplayUIRendererInit(true)) {
            editor::graphics::setReplayUIActive(false);
            record::Recorder::getInstance().setThumbnailCaptureProvider(nullptr);
            gController.setFrameTap(nullptr);
            editor::graphics::gImGuiRenderer.setContext(nullptr);
            ui::ReplayEditor::getInstance().shutdown();
            gContext.reset();
            Playback::getInstance().getSelf().getLogger().error(
                "Unable to install the early D3D12 renderer hook; the replay timeline may be unavailable"
            );
            return false;
        }
        if (!editor::graphics::hookD3D12(true)) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Replay ImGui timeline is unavailable; replay support will continue without it"
            );
        }
        if (!editor::graphics::hookReplayMouse(true)) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Replay mouse hook unavailable; timeline may not be interactive"
            );
        }
        return true;
    }

    editor::graphics::setReplayUIActive(false);
    editor::graphics::setReplayMouseInputActive(false);
    record::Recorder::getInstance().setThumbnailCaptureProvider(nullptr);
    gController.setFrameTap(nullptr);

    bool ok = true;
    if (!hookReplayUIRendererInit(false)) {
        Playback::getInstance().getSelf().getLogger().error("Unable to remove the early D3D12 renderer hook");
        ok = false;
    }
    if (!editor::graphics::hookD3D12(false)) {
        Playback::getInstance().getSelf().getLogger().error("Unable to remove replay ImGui timeline hooks");
        ok = false;
    }
    if (!editor::graphics::hookReplayMouse(false)) {
        Playback::getInstance().getSelf().getLogger().error("Unable to remove replay mouse hook");
        ok = false;
    }

    if (!ok) return false;

    ui::ReplayEditor::getInstance().shutdown();
    editor::graphics::gImGuiRenderer.setContext(nullptr);
    gController.reset();
    gContext.reset();
    return ok;
}

bool isReplayBrowserVisible() { return gContext.snapshot().browser.visible; }

void tickReplayExportBeforeClientUpdate() { gController.tickExportBeforeClientUpdate(); }

void tickReplayUI(bool hudVisible) {
    gController.tick(hudVisible);
    replay::ReplaySession::getInstance().updateObserverPreview();
}

void submitEditorAction(EditorAction action) { gContext.submit(std::move(action)); }

} // namespace playback::editor
