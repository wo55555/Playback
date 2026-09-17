#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace playback::editor::ui {

// Base rasterisation size; ImGui re-rasterises on style.FontScaleMain changes, so scaling stays sharp.
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

// Large is the size the editor was laid out against; Auto only departs from it for extreme displays.
[[nodiscard]] constexpr UiScaleTier resolveAutoTier(float displayHeight) {
    if (displayHeight <= 0.0f) return UiScaleTier::Large;
    if (displayHeight < 900.0f) return UiScaleTier::Medium;
    if (displayHeight < 2200.0f) return UiScaleTier::Large;
    return UiScaleTier::Huge;
}

[[nodiscard]] constexpr float calculateReplayUIScale(UiScaleTier tier, float displayHeight) {
    return uiScaleTierFactor(tier == UiScaleTier::Auto ? resolveAutoTier(displayHeight) : tier);
}

// Auto resolves to its concrete tier first, so stepping from Auto moves relative to what is on screen.
[[nodiscard]] constexpr UiScaleTier steppedUiScaleTier(UiScaleTier tier, int delta, float displayHeight) {
    auto const concrete = tier == UiScaleTier::Auto ? resolveAutoTier(displayHeight) : tier;
    int const  stepped  = std::clamp(
        static_cast<int>(concrete) + delta,
        static_cast<int>(UiScaleTier::Small),
        static_cast<int>(UiScaleTier::Huge)
    );
    return static_cast<UiScaleTier>(stepped);
}

// Set by the editor from persisted preferences; read by whichever backend drives the frame.
[[nodiscard]] UiScaleTier currentUiScaleTier() noexcept;
void                      setCurrentUiScaleTier(UiScaleTier tier) noexcept;

} // namespace playback::editor::ui
