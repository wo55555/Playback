#include "RecordDialogs.h"

#include "playback/Playback.h"
#include "playback/editor/graphics/ReplayUILayout.h"
#include "playback/editor/input/EditorInput.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/MessageDialog.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/record/Recorder.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <string>
#include <string_view>

namespace playback::screen {

namespace {

using namespace ll::i18n_literals;

// The dialog itself must not swallow Escape while a modal popup is open: input::routeKeyEvent
// keeps Escape with the UI, so the "record" game action never sees it and the pause menu cannot reopen.
class RecordDialogEscapeGuard {
public:
    RecordDialogEscapeGuard() { editor::input::setUiKeyboardCaptured(true); }
    ~RecordDialogEscapeGuard() { editor::input::setUiKeyboardCaptured(false); }

    RecordDialogEscapeGuard(RecordDialogEscapeGuard const&)            = delete;
    RecordDialogEscapeGuard& operator=(RecordDialogEscapeGuard const&) = delete;
};

std::atomic<RecordDialog> gDialog{RecordDialog::None};
std::atomic_bool          gOpened{false};

enum class Choice : int { None, Save, Delete, Confirm, Keep, Close };

std::atomic<Choice> gChoice{Choice::None};

std::string      gSaveNameBuffer;
std::atomic_bool gSaveNameInitialized{false};

[[nodiscard]] RecordDialog currentDialog() { return gDialog.load(std::memory_order_acquire); }

// Escape picks the first secondary button; the save dialog offers none so it cannot drop the pending recording.

// ConfirmCancel runs with an active recording.
[[nodiscard]] bool expectActiveRecording() {
    if (Playback::getInstance().isReplayMode()) return false;
    return record::Recorder::getInstance().isActive();
}

// SaveName and ConfirmDelete run with a finished recording waiting for a name.
[[nodiscard]] bool expectPendingRecording() {
    if (Playback::getInstance().isReplayMode()) return false;
    return record::Recorder::getInstance().hasPendingRecording();
}

void closeDialog() { gDialog.store(RecordDialog::None, std::memory_order_release); }

void drawConfirmCancelDialog() {
    std::string const id = "playback.recordControls.dialog.confirmCancelTitle"_tr();
    if (!editor::ui::beginMessageDialog(
            id.c_str(),
            ICON_WARNING,
            editor::ui::theme::kWarning,
            "playback.recordControls.dialog.confirmCancelTitle"_tr(),
            18.0f / editor::ui::kBaseFontSize
        ))
        return;
    editor::ui::dialogText("playback.recordControls.dialog.confirmCancelMessage"_tr());
    int const choice = editor::ui::dialogButtons({
        {"playback.recordControls.dialog.keep"_tr(),    editor::ui::DialogButtonKind::Secondary},
        {"playback.recordControls.dialog.discard"_tr(), editor::ui::DialogButtonKind::Danger   },
    });
    if (choice == 0) gChoice.store(Choice::Close, std::memory_order_release);
    else if (choice == 1) gChoice.store(Choice::Confirm, std::memory_order_release);
    editor::ui::endMessageDialog();
}

void drawSaveNameDialog() {
    if (!gSaveNameInitialized.exchange(true, std::memory_order_acq_rel)) {
        gSaveNameBuffer = record::Recorder::getInstance().pendingRecordingName();
    }
    std::string const id = "playback.recordControls.dialog.saveTitle"_tr();
    if (!editor::ui::beginMessageDialog(
            id.c_str(),
            ICON_SAVE,
            editor::ui::theme::kAccent,
            "playback.recordControls.dialog.saveTitle"_tr(),
            18.0f / editor::ui::kBaseFontSize
        ))
        return;
    editor::ui::dialogText("playback.recordControls.dialog.saveDescription"_tr(), true);

    std::array<char, 256> buffer{};
    std::copy_n(gSaveNameBuffer.data(), std::min(gSaveNameBuffer.size(), buffer.size() - 1), buffer.data());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, editor::ui::theme::kButton);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, editor::ui::theme::kButtonHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, editor::ui::theme::kButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, editor::ui::theme::kText);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.0f, 10.0f});
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool const edited =
        ImGui::InputText("##record-save-name", buffer.data(), buffer.size(), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (edited) gSaveNameBuffer = buffer.data();

    int const choice = editor::ui::dialogButtons({
        {"playback.recordControls.dialog.delete"_tr(), editor::ui::DialogButtonKind::Danger },
        {"playback.recordControls.dialog.save"_tr(),   editor::ui::DialogButtonKind::Primary},
    });
    if (choice == 0) gChoice.store(Choice::Delete, std::memory_order_release);
    else if (choice == 1) gChoice.store(Choice::Save, std::memory_order_release);
    editor::ui::endMessageDialog();
}

void drawConfirmDeleteDialog() {
    std::string const id = "playback.recordControls.dialog.confirmDeleteTitle"_tr();
    if (!editor::ui::beginMessageDialog(
            id.c_str(),
            ICON_TRASH,
            editor::ui::theme::kError,
            "playback.recordControls.dialog.confirmDeleteTitle"_tr(),
            18.0f / editor::ui::kBaseFontSize
        ))
        return;
    editor::ui::dialogText("playback.recordControls.dialog.confirmDeleteMessage"_tr());
    int const choice = editor::ui::dialogButtons({
        {"playback.recordControls.dialog.back"_tr()},
        {"playback.recordControls.dialog.delete"_tr(), editor::ui::DialogButtonKind::Danger},
    });
    if (choice == 0) gChoice.store(Choice::Keep, std::memory_order_release);
    else if (choice == 1) gChoice.store(Choice::Confirm, std::memory_order_release);
    editor::ui::endMessageDialog();
}

} // namespace

void openRecordDialog(RecordDialog dialog) {
    // Each finished recording starts from its own timestamp name.
    if (dialog == RecordDialog::SaveName) gSaveNameInitialized.store(false, std::memory_order_release);
    gChoice.store(Choice::None, std::memory_order_release);
    gDialog.store(dialog, std::memory_order_release);
    gOpened.store(true, std::memory_order_release);
}

bool isRecordDialogVisible() { return currentDialog() != RecordDialog::None; }

void drawRecordDialogs() {
    auto const dialog = currentDialog();
    if (dialog == RecordDialog::None) return;

    if (gOpened.exchange(false, std::memory_order_acq_rel)) {
        switch (dialog) {
        case RecordDialog::ConfirmCancel:
            ImGui::OpenPopup("playback.recordControls.dialog.confirmCancelTitle"_tr().c_str());
            break;
        case RecordDialog::SaveName:
            ImGui::OpenPopup("playback.recordControls.dialog.saveTitle"_tr().c_str());
            break;
        case RecordDialog::ConfirmDelete:
            ImGui::OpenPopup("playback.recordControls.dialog.confirmDeleteTitle"_tr().c_str());
            break;
        default:
            break;
        }
    }

    RecordDialogEscapeGuard guard;
    switch (dialog) {
    case RecordDialog::ConfirmCancel:
        drawConfirmCancelDialog();
        break;
    case RecordDialog::SaveName:
        drawSaveNameDialog();
        break;
    case RecordDialog::ConfirmDelete:
        drawConfirmDeleteDialog();
        break;
    default:
        break;
    }
}

void tickRecordDialogs() {
    auto const dialog = currentDialog();
    if (dialog == RecordDialog::None) return;

    // The recording these dialogs act on is gone (level exit, /record stop or discard): drop them.
    bool const stale = (dialog == RecordDialog::ConfirmCancel && !expectActiveRecording())
                    || (dialog == RecordDialog::ConfirmDelete && !expectPendingRecording())
                    || (dialog == RecordDialog::SaveName && !expectPendingRecording());
    if (stale) {
        closeDialog();
        return;
    }

    auto& recorder = record::Recorder::getInstance();
    switch (gChoice.exchange(Choice::None, std::memory_order_acq_rel)) {
    case Choice::Save:
        if (recorder.savePendingRecording(gSaveNameBuffer)) closeDialog();
        break;
    case Choice::Delete:
        if (recorder.hasPendingRecording()) {
            gDialog.store(RecordDialog::ConfirmDelete, std::memory_order_release);
            gOpened.store(true, std::memory_order_release);
        } else {
            closeDialog();
        }
        break;
    case Choice::Confirm:
        if (dialog == RecordDialog::ConfirmCancel) recorder.discard();
        else recorder.discardPendingRecording();
        closeDialog();
        break;
    case Choice::Keep:
        gDialog.store(RecordDialog::SaveName, std::memory_order_release);
        gOpened.store(true, std::memory_order_release);
        break;
    case Choice::Close:
        closeDialog();
        break;
    default:
        break;
    }
}

} // namespace playback::screen
