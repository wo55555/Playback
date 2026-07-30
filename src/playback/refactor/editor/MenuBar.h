#pragma once

namespace playback::refactor::editor {

class MenuBar {
public:
    void draw();
    [[nodiscard]] bool isAnyMenuOpen() const;

private:
    bool mExportDialogOpen{false};
};

} // namespace playback::refactor::editor
