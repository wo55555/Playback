#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/keyframe/CameraRenderState.h"

#include "imgui.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace playback::editor::ui {

class CameraPathOverlay {
public:
    void               draw(PanelContext const& ctx, Rect const& videoRect, ImDrawList* drawList);
    void               clear();
    [[nodiscard]] bool isBuilding() const { return mNextPath < mPaths.size(); }

private:
    struct Polyline {
        size_t                   dimensionSegment{};
        std::vector<::glm::vec3> points;
        int                      fromTick{};
        int                      toTick{};
        int64_t                  steps{};
    };

    struct Marker {
        int                         tick{};
        size_t                      dimensionSegment{};
        keyframe::CameraRenderState pose{};
    };

    struct CameraPath {
        std::string           cameraId;
        std::vector<Polyline> polylines;
        std::vector<Marker>   markers;
    };

    void rebuild(keyframe::CameraTimelineHandle const& timeline);
    void advanceBuild();

    keyframe::CameraTimelineHandle mTimeline;
    std::vector<CameraPath>        mPaths;
    size_t                         mNextPath{};
    size_t                         mNextLine{};
};

} // namespace playback::editor::ui
