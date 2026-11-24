#pragma once

#include "playback/editor/ui/EditorTheme.h"

#include <functional>
#include <string_view>

namespace playback::editor::ui::property {

enum class ButtonVariant {
    Secondary,
    Primary,
    Warning,
    Danger,
    Subtle,
};

enum class TriState {
    Off,
    On,
    Mixed,
};

struct ActionButtonSpec {
    char const*   label{};
    char const*   icon{};
    ButtonVariant variant{ButtonVariant::Secondary};
    bool          enabled{true};
    char const*   tooltip{};
};

struct InspectorHeaderOptions {
    ImU32                 accentColor{theme::kAccent};
    std::string_view      badgeText{};
    ImU32                 badgeColor{theme::kAccent};
    float                 rightSlotWidth{0.0f};
    std::function<void()> renderRightSlot{};
};

struct CardOptions {
    bool        defaultOpen{true};
    bool        collapsible{true};
    char const* badgeText{};
    ImU32       badgeColor{theme::kAccent};
    bool*       toggleValue{nullptr};
    bool*       toggleChanged{nullptr};
    bool        toggleDisabled{false};
};

void beginInspector(std::string_view title, std::string_view objectName);
void beginInspector(std::string_view title, std::string_view subtitle, InspectorHeaderOptions const& options);
[[nodiscard]] float inspectorHeaderIconSlotWidth(int buttonCount);
// Begins a bordered dark card container; pair with endCard() only when this returns true.
bool beginCard(char const* id, char const* title, CardOptions const& options = {});
void endCard();
bool beginSection(char const* label, bool defaultOpen = true);
void endSection();
void statusPill(char const* text, ImU32 accentColor, bool rightAligned = false);
bool triStateCheckbox(char const* label, TriState state, char const* mixedHint = nullptr);
void beginDangerFooter();
void endDangerFooter();
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
