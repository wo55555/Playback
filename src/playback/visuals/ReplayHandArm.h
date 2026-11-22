#pragma once

#include "mc/world/item/ItemStack.h"

#include <algorithm>

namespace playback::visuals {

inline constexpr float HandMaxHeightStep = 0.4f;
inline constexpr float HandSwapHeight    = 0.1f;
inline constexpr int   HandOffhandSlot   = 0;

// One hand's raise/lower state, stepped once per tick like the native ItemInHandRenderer::tick.
struct HandArm {
    ItemStack item;
    int       slot{-1};
    float     height{1.0f};
    float     oldHeight{1.0f};
};

inline bool sameHandItem(ItemStack const& a, ItemStack const& b) {
    // matchesItem also differs when a raised shield's blocking tick changes; native keeps the hand up.
    return a.isNull() ? b.isNull() : !b.isNull() && a.sameItemAndAux(b);
}

inline void snapHandArm(HandArm& arm, ItemStack const& item, int slot) {
    arm.item      = item;
    arm.slot      = slot;
    arm.height    = 1.0f;
    arm.oldHeight = 1.0f;
}

inline void stepHandArm(HandArm& arm, ItemStack const& item, int slot) {
    arm.oldHeight = arm.height;
    // An empty hand ignores the slot, as the native renderer does.
    bool const  same    = sameHandItem(arm.item, item) && (item.isNull() || arm.slot == slot);
    float const target  = same ? 1.0f : 0.0f;
    arm.height         += std::clamp(target - arm.height, -HandMaxHeightStep, HandMaxHeightStep);
    // The comparator ignores NBT, durability and blocking state; keep the rendered stack current.
    if (same) arm.item = item;
    if (arm.height < HandSwapHeight) {
        arm.item = item;
        arm.slot = slot;
    }
}

} // namespace playback::visuals
