#pragma once

#include <string>

namespace playback::refactor::editor {

class RenderMode {
public:
    void draw();

private:
    int    mCurrentFrame{};
    int    mTotalFrames{};
    int    mProgressPercent{};
    std::string mOutputPath;
    std::string mEta;
};

} // namespace playback::refactor::editor