#pragma once

namespace playback::refactor::editor {

class MenuBar {
public:
    void draw();
    [[nodiscard]] bool isAnyMenuOpen() const;
};

} // namespace playback::refactor::editor