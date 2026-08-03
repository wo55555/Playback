#pragma once

#include <string>

namespace playback::editor::ui {

class RenderMode {
public:
    void draw();

private:
    int         mCurrentFrame{};
    int         mTotalFrames{};
    int         mProgressPercent{};
    std::string mOutputPath;
    std::string mEta;
};

} // namespace playback::editor::ui