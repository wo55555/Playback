#include "WindowCloseGuard.h"

#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/exporting/ExportActivity.h"
#include "playback/replay/ReplaySession.h"

#include <Windows.h>
#include <atomic>

namespace playback::runtime {

namespace {

std::atomic<HWND> gCloseWindow{};
std::atomic_bool  gCloseReady{};
bool              gStopRequested{}; // Client thread only.

} // namespace

bool deferWindowClose(void* window) {
    if (gCloseReady.exchange(false, std::memory_order_acq_rel)) {
        gCloseWindow.store(nullptr, std::memory_order_release);
        return false;
    }
    gCloseWindow.store(static_cast<HWND>(window), std::memory_order_release);
    return true;
}

void advanceWindowClose() {
    auto const window = gCloseWindow.load(std::memory_order_acquire);
    if (!window || gCloseReady.load(std::memory_order_acquire)) return;

    auto& session = replay::ReplaySession::getInstance();
    if (session.isIsolatingReplayWorld() || session.isReplayWorldCleanupPending()
        || exporting::isExportActivityActive()) {
        if (!gStopRequested) {
            editor::submitEditorAction({state::EditorActionType::StopReplay});
            gStopRequested = true;
            Playback::getInstance().getSelf().getLogger().debug(
                "Waiting for replay world cleanup before closing the window"
            );
        }
        return;
    }

    gStopRequested = false;
    gCloseReady.store(true, std::memory_order_release);
    if (!PostMessageW(window, WM_CLOSE, 0, 0)) {
        gCloseReady.store(false, std::memory_order_release);
        gCloseWindow.store(nullptr, std::memory_order_release);
        Playback::getInstance().getSelf().getLogger().error("Unable to resume closing the window after replay cleanup");
    }
}

} // namespace playback::runtime
