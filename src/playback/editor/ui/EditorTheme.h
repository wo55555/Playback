#pragma once

#include "imgui.h"

namespace playback::editor::ui::theme {

// Single source of truth for editor colours; panels reference these instead of local literals.
constexpr ImU32 kBgPanel  = IM_COL32(0x1a, 0x1a, 0x1a, 0xff);
constexpr ImU32 kBgHeader = IM_COL32(0x25, 0x25, 0x25, 0xff);
constexpr ImU32 kBorder   = IM_COL32(0x3a, 0x3a, 0x3a, 0xff);
constexpr ImU32 kText     = IM_COL32(0xe0, 0xe0, 0xe0, 0xff);
constexpr ImU32 kTextDim  = IM_COL32(0x90, 0x90, 0x90, 0xff);
constexpr ImU32 kAccent   = IM_COL32(0x3a, 0x8c, 0xf0, 0xff);
constexpr ImU32 kSelected = IM_COL32(0xf0, 0xc0, 0x20, 0xff);
constexpr ImU32 kHover    = IM_COL32(0x3a, 0x5a, 0x8c, 0x99);
constexpr ImU32 kSuccess  = IM_COL32(0x3a, 0xf0, 0x3a, 0xff);
constexpr ImU32 kWarning  = IM_COL32(0xf0, 0xc0, 0x20, 0xff);

// Timeline surfaces.
constexpr ImU32 kTimelineBg        = IM_COL32(27, 27, 27, 255);
constexpr ImU32 kTimelineSidebar   = IM_COL32(41, 41, 41, 255);
constexpr ImU32 kTimelineRuler     = IM_COL32(32, 32, 32, 255);
constexpr ImU32 kTimelineTrackBg   = IM_COL32(29, 29, 29, 255);
constexpr ImU32 kLine              = IM_COL32(73, 73, 73, 255);
constexpr ImU32 kCameraColor       = IM_COL32(77, 63, 83, 255);
constexpr ImU32 kCameraSelected    = IM_COL32(112, 87, 124, 255);
constexpr ImU32 kRowHighlight      = IM_COL32(58, 58, 58, 255);
constexpr ImU32 kRowSelectedBorder = IM_COL32(122, 142, 166, 255);

// Inspector surfaces.
constexpr ImU32 kInspectorHeader = IM_COL32(32, 32, 32, 255);
constexpr ImU32 kSection         = IM_COL32(45, 45, 45, 255);
constexpr ImU32 kSectionHover    = IM_COL32(56, 56, 56, 255);
constexpr ImU32 kFineDivider     = IM_COL32(66, 66, 66, 210);

// Viewport and icon surfaces.
constexpr ImU32 kViewportBg    = IM_COL32(0x0d, 0x0d, 0x0d, 0xff);
constexpr ImU32 kIconActive    = IM_COL32(230, 232, 238, 255);
constexpr ImU32 kIconInactive  = IM_COL32(170, 170, 170, 255);
constexpr ImU32 kIconHighlight = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kOverlayBg     = IM_COL32(20, 20, 24, 210);
constexpr ImU32 kOverlayHover  = IM_COL32(58, 90, 140, 235);

constexpr float kPanelPadding     = 8.0f;
constexpr float kItemSpacing      = 4.0f;
constexpr float kFrameRounding    = 4.0f;
constexpr float kFineDividerWidth = 0.75f;

void apply();

} // namespace playback::editor::ui::theme
