#pragma once

namespace playback::screen {

enum class RecordDialog { None, ConfirmCancel, SaveName, ConfirmDelete };

// Client thread. SaveName takes its default name from the recorder's pending recording.
void               openRecordDialog(RecordDialog dialog);
[[nodiscard]] bool isRecordDialogVisible();

// Render thread, inside an ImGui frame.
void drawRecordDialogs();

// Client thread: applies the choices made in the dialogs and closes ones whose recording is gone.
void tickRecordDialogs();

} // namespace playback::screen
