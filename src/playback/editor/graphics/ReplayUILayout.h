#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace playback::editor::ui {

// Fonts are rasterised at this size; ImGui re-rasterises them when style.FontScaleMain changes, so the
// whole editor scales without blurring (imgui 1.92 dynamic fonts + ImGuiBackendFlags_RendererHasTextures).
inline constexpr float kBaseFontSize = 14.0f;

enum class UiScaleTier : uint8_t { Auto = 0, Small, Medium, Large, Huge };

[[nodiscard]] constexpr std::string_view uiScaleTierName(UiScaleTier tier) {
    switch (tier) {
    case UiScaleTier::Small:
        return "small";
    case UiScaleTier::Medium:
        return "medium";
    case UiScaleTier::Large:
        return "large";
    case UiScaleTier::Huge:
        return "huge";
    case UiScaleTier::Auto:
    default:
        return "auto";
    }
}

[[nodiscard]] constexpr UiScaleTier uiScaleTierFromName(std::string_view name) {
    if (name == "small") return UiScaleTier::Small;
    if (name == "medium") return UiScaleTier::Medium;
    if (name == "large") return UiScaleTier::Large;
    if (name == "huge") return UiScaleTier::Huge;
    return UiScaleTier::Auto;
}

[[nodiscard]] constexpr float uiScaleTierFactor(UiScaleTier tier) {
    switch (tier) {
    case UiScaleTier::Medium:
        return 1.25f;
    case UiScaleTier::Large:
        return 1.5f;
    case UiScaleTier::Huge:
        return 1.75f;
    default:
        return 1.0f;
    }
}

// A 22px control is physically small on a short panel, so low screens scale up rather than down; tall
// screens scale up because their pixels are dense. 1080p is the 1.0 reference.
[[nodiscard]] constexpr UiScaleTier resolveAutoTier(float displayHeight) {
    if (displayHeight <= 0.0f) return UiScaleTier::Small;
    if (displayHeight < 800.0f) return UiScaleTier::Large;
    if (displayHeight < 900.0f) return UiScaleTier::Medium;
    if (displayHeight < 1300.0f) return UiScaleTier::Small;
    if (displayHeight < 1700.0f) return UiScaleTier::Medium;
    return UiScaleTier::Large;
}

[[nodiscard]] constexpr float calculateReplayUIScale(UiScaleTier tier, float displayHeight) {
    return uiScaleTierFactor(tier == UiScaleTier::Auto ? resolveAutoTier(displayHeight) : tier);
}

// Set by the editor from persisted preferences; read by whichever backend drives the frame.
[[nodiscard]] UiScaleTier currentUiScaleTier() noexcept;
void                      setCurrentUiScaleTier(UiScaleTier tier) noexcept;

} // namespace playback::editor::ui
