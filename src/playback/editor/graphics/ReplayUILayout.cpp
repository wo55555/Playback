#include "playback/editor/graphics/ReplayUILayout.h"

#include <atomic>

namespace playback::editor::ui {

namespace {

// Read on the render thread, written from the UI thread when the preference changes.
std::atomic<UiScaleTier> gUiScaleTier{UiScaleTier::Auto};

} // namespace

UiScaleTier currentUiScaleTier() noexcept { return gUiScaleTier.load(std::memory_order_acquire); }

void setCurrentUiScaleTier(UiScaleTier tier) noexcept { gUiScaleTier.store(tier, std::memory_order_release); }

} // namespace playback::editor::ui
