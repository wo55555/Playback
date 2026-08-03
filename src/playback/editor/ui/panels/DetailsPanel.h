#pragma once

#include <string>

namespace playback::editor::ui {

class DetailsPanel {
public:
    void draw();

private:
    std::string mSpeedSegmentId;
    float       mSpeedValue{};
    bool        mSpeedEditActive{};
    std::string mTickKeyframeId;
    int         mTickValue{};
    bool        mTickEditActive{};
};

} // namespace playback::editor::ui
