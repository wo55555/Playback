#pragma once

#include <string>

namespace playback::refactor::editor {

// Central key binding registry
class KeyMap {
public:
    // Check if a Windows virtual key matches a named action
    static bool matches(const std::string& actionName, WPARAM wParam);

    // Initialize all default bindings
    static void initialize();

    // Get display string for a shortcut (e.g. "Ctrl+Z")
    static std::string displayString(const std::string& actionName);
};

} // namespace playback::refactor::editor