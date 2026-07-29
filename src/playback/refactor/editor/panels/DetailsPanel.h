#pragma once

#include "playback/refactor/editor/models/CameraKeyframe.h"
#include "playback/refactor/editor/models/Track.h"

#include <functional>
#include <string>
#include <vector>

namespace playback::refactor::editor {

class DetailsPanel {
public:
    void draw();

private:
    void drawEmpty();
    void drawCameraTrack();
    void drawKeyframe();
    void drawClip();
    void drawMarker();
    void drawTransition();

    // Field editors
    void drawNumberField(std::string_view label, float& v, float step = 0.1f);
    void drawVec3Field(std::string_view label, Vec3& v);
    void drawAngleField(std::string_view label, float& deg);
    void drawColorField(Color4& c);
    void drawDropdown(std::string_view label, std::string_view current, const std::vector<std::string>& options, int& idx);
    void drawButton(std::string_view label, std::string_view icon, std::function<void()> onClick);
};

} // namespace playback::refactor::editor