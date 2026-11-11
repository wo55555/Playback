#include "RecordControlsHooks.h"

#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/exporting/ExportActivity.h"
#include "playback/record/Recorder.h"
#include "playback/replay/ReplaySession.h"
#include "playback/runtime/GameAddress.h"
#include "playback/screen/RecordDialogs.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/DirtyFlag.h"
#include "mc/client/gui/ViewRequest.h"
#include "mc/client/gui/controls/UIPropertyBag.h"
#include "mc/client/gui/screens/controllers/HudScreenController.h"
#include "mc/client/gui/screens/controllers/PauseScreenController.h"
#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/deps/core/string/StringHash.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace playback::screen {

namespace {

using namespace ll::i18n_literals;

constexpr std::string_view kButtonToggle = "button.playback_record_toggle";
constexpr std::string_view kButtonFinish = "button.playback_record_finish";
constexpr std::string_view kButtonCancel = "button.playback_record_cancel";

std::atomic_bool gIndicatorEnabled{true};
int64_t          gLastHudSignature = -1;

enum class RecordUiState : int { Hidden, Idle, Recording, Paused };

// Replay preview, the editor and export all run inside the replay session, which may outlive the replay level.
[[nodiscard]] bool replayContextActive() {
    auto const& session = replay::ReplaySession::getInstance();
    return Playback::getInstance().isReplayMode() || session.isActive() || session.isReplayWorldCleanupPending()
        || exporting::isExportActivityActive() || exporting::isOfflineRenderActivityActive();
}

RecordUiState recordUiState() {
    if (replayContextActive()) return RecordUiState::Hidden;
    auto const& recorder = record::Recorder::getInstance();
    if (!recorder.isActive()) return RecordUiState::Idle;
    return recorder.isPaused() ? RecordUiState::Paused : RecordUiState::Recording;
}

bool indicatorVisible() {
    auto const state = recordUiState();
    return gIndicatorEnabled.load(std::memory_order_relaxed)
        && (state == RecordUiState::Recording || state == RecordUiState::Paused)
        && !record::Recorder::getInstance().isThumbnailPending();
}

int recordedSeconds() { return record::Recorder::getInstance().recordedTicks() / 20; }

std::string formatRecordedTime() {
    int const total = recordedSeconds();
    int const hours = total / 3600;
    if (hours > 0) return std::format("{}:{:02}:{:02}", hours, total / 60 % 60, total % 60);
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

int64_t hudSignature() {
    if (!indicatorVisible()) return 0;
    return int64_t{recordedSeconds()} * 4 + static_cast<int>(recordUiState());
}

// Mirrors the vanilla "return to game" handler, which leaves the screen and then requests Exit.
::ui::ViewRequest closePauseScreen(PauseScreenController& ctrl) {
    std::shared_ptr<MinecraftScreenModel> const& model = ctrl.mMinecraftScreenModel;
    if (model) model->leaveScreen(std::string{}, false);
    return ::ui::ViewRequest::Exit;
}

// The save/cancel flows need the ImGui dialog layer; without it the recorder falls back to a direct save.
[[nodiscard]] bool recordDialogsAvailable() { return editor::isReplayUiHooked(); }

void registerPauseControls(PauseScreenController& ctrl) {
    ctrl.bindBool(
        StringHash("#playback_record_controls_visible"),
        [] { return recordUiState() != RecordUiState::Hidden; },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_record_active"),
        [] {
            auto const state = recordUiState();
            return state == RecordUiState::Recording || state == RecordUiState::Paused;
        },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_record_paused"),
        [] { return recordUiState() == RecordUiState::Paused; },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_record_idle"),
        [] { return recordUiState() == RecordUiState::Idle; },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_record_recording"),
        [] { return recordUiState() == RecordUiState::Recording; },
        [] { return true; }
    );

    auto* const controller = &ctrl;
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonToggle)), [controller](UIPropertyBag*) {
        auto& recorder = record::Recorder::getInstance();
        switch (recordUiState()) {
        case RecordUiState::Hidden:
            return ::ui::ViewRequest::ConsumeEvent;
        case RecordUiState::Recording:
            recorder.pause();
            break;
        default:
            recorder.start();
            break;
        }
        return closePauseScreen(*controller);
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonFinish)), [controller](UIPropertyBag*) {
        auto& recorder = record::Recorder::getInstance();
        if (!recorder.isActive()) return ::ui::ViewRequest::ConsumeEvent;
        if (!recordDialogsAvailable()) {
            recorder.stop();
            return closePauseScreen(*controller);
        }
        if (recorder.finishToPending()) openRecordDialog(RecordDialog::SaveName);
        return closePauseScreen(*controller);
    });
    ctrl.registerButtonPressedHandler(ctrl._getNameId(std::string(kButtonCancel)), [controller](UIPropertyBag*) {
        auto& recorder = record::Recorder::getInstance();
        if (!recorder.isActive()) return ::ui::ViewRequest::ConsumeEvent;
        if (recordDialogsAvailable()) {
            openRecordDialog(RecordDialog::ConfirmCancel);
        } else {
            recorder.discard();
        }
        return closePauseScreen(*controller);
    });
}

void registerHudIndicator(HudScreenController& ctrl) {
    ctrl.bindBool(
        StringHash("#playback_record_indicator_visible"),
        [] { return indicatorVisible(); },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_record_is_recording"),
        [] { return recordUiState() == RecordUiState::Recording; },
        [] { return true; }
    );
    ctrl.bindBool(
        StringHash("#playback_record_is_paused"),
        [] { return recordUiState() == RecordUiState::Paused; },
        [] { return true; }
    );
    ctrl.bindString(
        StringHash("#playback_record_status_text"),
        []() -> std::string {
            return recordUiState() == RecordUiState::Paused ? "playback.recordControls.paused"_tr()
                                                            : "playback.recordControls.recording"_tr();
        },
        [] { return true; }
    );
    ctrl.bindString(StringHash("#playback_record_time_text"), [] { return formatRecordedTime(); }, [] { return true; });
}

} // namespace

// This SDK does not export the controllers' _registerEventHandlers; register once their constructors have run.
LL_TYPE_INSTANCE_HOOK(
    PauseRecordControlsHook,
    ll::memory::HookPriority::Normal,
    PauseScreenController,
    &PauseScreenController::$ctor,
    void*,
    std::shared_ptr<PauseScreenModel> model,
    bool                              overlaySocialDrawerOnce
) {
    auto* result = origin(std::move(model), overlaySocialDrawerOnce);
    registerPauseControls(*this);
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    HudRecordIndicatorBindHook,
    ll::memory::HookPriority::Normal,
    HudScreenController,
    &HudScreenController::$ctor,
    void*,
    std::shared_ptr<ClientInstanceScreenModel> model
) {
    auto* result = origin(std::move(model));
    registerHudIndicator(*this);
    gLastHudSignature = -1;
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    HudRecordIndicatorTickHook,
    ll::memory::HookPriority::Normal,
    HudScreenController,
    runtime::gameAddress(runtime::GameFunction::HudScreenTick),
    ::ui::DirtyFlag
) {
    auto result = origin();
    // HUD bindings only re-evaluate on a dirty tick; refresh once per displayed second or state change.
    auto const signature = hudSignature();
    if (signature != gLastHudSignature) {
        gLastHudSignature = signature;
        result =
            static_cast<::ui::DirtyFlag>(static_cast<uint>(result) | static_cast<uint>(::ui::DirtyFlag::UpdateBinds));
    }
    return result;
}

void hookRecordControls(bool enable) {
    static bool pauseHooked = false;
    static bool hudHooked   = false;
    static bool tickHooked  = false;
    if (enable) {
        auto& logger = Playback::getInstance().getSelf().getLogger();
        if (!pauseHooked) pauseHooked = PauseRecordControlsHook::hook() == 0;
        if (!hudHooked) hudHooked = HudRecordIndicatorBindHook::hook() == 0;
        if (!tickHooked) tickHooked = HudRecordIndicatorTickHook::hook() == 0;
        if (!pauseHooked) logger.warn("Pause-menu recording controls are unavailable");
        if (!hudHooked || !tickHooked) logger.warn("Recording indicator is unavailable");
        return;
    }
    if (tickHooked) tickHooked = !HudRecordIndicatorTickHook::unhook();
    if (hudHooked) hudHooked = !HudRecordIndicatorBindHook::unhook();
    if (pauseHooked) pauseHooked = !PauseRecordControlsHook::unhook();
    gLastHudSignature = -1;
}

void setRecordingIndicatorEnabled(bool enabled) { gIndicatorEnabled.store(enabled, std::memory_order_relaxed); }

bool isRecordingIndicatorEnabled() { return gIndicatorEnabled.load(std::memory_order_relaxed); }

} // namespace playback::screen
