#pragma once

#include "imgui.h"

#include <initializer_list>
#include <string_view>

namespace playback::editor::ui {

enum class DialogButtonKind { Secondary, Primary, Danger };

struct DialogButton {
    std::string_view label;
    DialogButtonKind kind     = DialogButtonKind::Secondary;
    bool             disabled = false;
};

// Modal with an icon badge beside the title; `fontScale` is the dialog's own window font scale.
[[nodiscard]] bool
     beginMessageDialog(char const* id, char const* icon, ImU32 tone, std::string_view title, float fontScale = 1.0f);
void dialogText(std::string_view text, bool secondary = false);
// Right-aligned row; returns the clicked index. Enter picks the first primary button, Escape the first secondary.
[[nodiscard]] int dialogButtons(std::initializer_list<DialogButton> buttons);
void              endMessageDialog();

} // namespace playback::editor::ui
