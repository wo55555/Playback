// Lucide icon font codepoints
// Source font: resources/fonts/lucide.ttf (Lucide v1.17+ from unpkg.com/lucide-static)
// Generated from resources/fonts/lucide.css
// License: ISC (Lucide) - https://lucide.dev/

#pragma once

namespace Playback::ReplayEditor::Icons {

// Font family name in TTF
constexpr const char* kFontFamily = "lucide";

// Font file path (relative to executable)
constexpr const char* kFontPath = "resources/fonts/lucide.ttf";

// Time icons
#define ICON_PLAY   "\ue13c" // play
#define ICON_PAUSE  "\ue12e" // pause
#define ICON_STOP   "\ue167" // square
#define ICON_RECORD "\ue13c" // fallback: play (record not in editor)

// Marker / keyframe
#define ICON_ADD_KEYFRAME "\ue5e2" // diamond-plus
#define ICON_KEYFRAME     "\ue2d2" // diamond
#define ICON_ADD_MARKER   "\ue613" // map-pin-plus
#define ICON_MARKER       "\ue111" // map-pin

// Edit operations
#define ICON_SPLIT "\ue14e" // scissors
#define ICON_UNDO  "\ue2a1" // undo-2
#define ICON_REDO  "\ue2a0" // redo-2
#define ICON_CUT   "\ue14e" // scissors (fallback)
#define ICON_COPY  "\ue09e" // copy
#define ICON_PASTE "\ue09e" // copy (fallback)

// File
#define ICON_OPEN       "\ue247" // folder-open
#define ICON_SAVE       "\ue14d" // save
#define ICON_EXPORT     "\ue19e" // upload
#define ICON_FILE       "\ue0c0" // file
#define ICON_FILE_VIDEO "\ue321" // file-video

// Generic actions
#define ICON_ADD    "\ue13d" // plus
#define ICON_DELETE "\ue18e" // trash-2
#define ICON_TRASH  "\ue18e" // trash-2

// State toggles
#define ICON_LOCK    "\ue10b" // lock
#define ICON_MUTE    "\ue1ac" // volume-x
#define ICON_HIDE    "\ue0bb" // eye-off
#define ICON_EYE     "\ue0ba" // eye
#define ICON_VISIBLE "\ue0ba" // eye

// UI controls
#define ICON_DRAG         "\ue0eb" // grip-vertical
#define ICON_CHEVRON_DOWN "\ue06d" // chevron-down
#define ICON_CHECK        "\ue06c" // check
#define ICON_SEARCH       "\ue151" // search
#define ICON_RESET        "\ue148" // rotate-ccw
#define ICON_CAMERA       "\ue1a5" // video
#define ICON_VIDEO        "\ue1a5" // video
#define ICON_SETTINGS     "\ue30b" // cog
#define ICON_REFRESH      "\ue145" // refresh-cw
#define ICON_HELP         "\ue082" // help-circle
#define ICON_INFO         "\ue0f9" // info
#define ICON_CLOSE        "\ue1b2" // x
#define ICON_MOVE         "\ue121" // move
#define ICON_FILE_PLUS    "\ue0c9" // file-plus
#define ICON_LIST         "\ue106" // list
#define ICON_GRID         "\ue0ff" // layout-grid
#define ICON_MORE         "\ue0b7" // ellipsis-vertical
#define ICON_SORT         "\ue37d" // arrow-up-down
#define ICON_FILTER       "\ue0dc" // filter
#define ICON_CALENDAR     "\ue063" // calendar
#define ICON_CLOCK        "\ue250" // clock-3
#define ICON_WORLD        "\ue0e8" // globe
#define ICON_WARNING      "\ue193" // triangle-alert

// Track status
#define ICON_TRACK_ACTIVE "\ue345" // circle-dot
#define ICON_TRACK_OFF    "\ue076" // circle

// Transitions
#define ICON_TRANSITION "\ue417" // arrow-right-left

// Render / loading state
#define ICON_RENDER "\ue10a" // loader-circle (rotating)
#define ICON_LOADER "\ue109" // loader

// Menu layout
#define ICON_PANEL_LEFT "\ue12a" // panel-left
#define ICON_BACK       "\ue048" // arrow-left (←)

} // namespace Playback::ReplayEditor::Icons
