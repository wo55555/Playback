#pragma once

#include <string>

namespace playback::refactor::editor {

class StatusPanel {
public:
    void draw();

private:
    // Cached display values
    std::string mModeText{"[Edit]"};
    std::string mProjectName;
    int         mFps{60};
    size_t      mMemoryMB{256};
};

} // namespace playback::refactor::editor