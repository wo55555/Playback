#pragma once

#include "playback/editor/ui/EditorTheme.h"

#include <string_view>

namespace playback::editor::ui::property {

enum class ButtonVariant {
    Secondary,
    Primary,
    Danger,
    Subtle,
};

struct ActionButtonSpec {
    char const*   label{};
    char const*   icon{};
    ButtonVariant variant{ButtonVariant::Secondary};
    bool          enabled{true};
    char const*   tooltip{};
};

void beginInspector(std::string_view title, std::string_view objectName);
bool beginSection(char const* label, bool defaultOpen = true);
void endSection();
void textRow(char const* label, char const* value);
// Splits a localized "Label: Value" or "标签：值" string into a styled two-column textRow.
void formattedRow(std::string_view text);
void separator();
bool actionButton(
    char const*   label,
    bool          enabled = true,
    ButtonVariant variant = ButtonVariant::Secondary,
    char const*   icon    = nullptr,
    char const*   tooltip = nullptr
);
// Draws two buttons side-by-side in equal halves when both labels fit; stacks vertically in narrow panels.
// Returns 0 if the first button was clicked, 1 if the second was clicked, or -1 otherwise.
int actionButtonPair(ActionButtonSpec const& first, ActionButtonSpec const& second);
bool choiceChip(char const* label, bool active, float width = -1.0f, ImU32 accent = theme::kAccent);

} // namespace playback::editor::ui::property
