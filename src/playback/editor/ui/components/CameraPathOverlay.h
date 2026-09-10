#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/keyframe/CameraRenderState.h"

#include "imgui.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace playback::editor::ui {

class CameraPathOverlay {
public:
    void               draw(PanelContext const& ctx, Rect const& videoRect, ImDrawList* drawList);
    void               clear();
    [[nodiscard]] bool isBuilding() const { return mNextLine < mPolylines.size(); }

private:
    struct PathWindow {
        int lastLast{-1};
        int last{-1};
        int next{-1};
        int nextNext{-1};

        [[nodiscard]] bool operator==(PathWindow const&) const = default;
    };

    struct Polyline {
        size_t                   dimensionSegment{};
        std::vector<::glm::vec3> points;
        int                      fromTick{};
        int                      toTick{};
        int64_t                  steps{};
        float                    opacity{1.0f};
    };

    struct Marker {
        int                         tick{};
        size_t                      dimensionSegment{};
        keyframe::CameraRenderState pose{};
        float                       opacity{1.0f};
    };

    void rebuild(keyframe::CameraTimelineHandle const& timeline, PathWindow const& window);
    void advanceBuild();

    keyframe::CameraTimelineHandle mTimeline;
    PathWindow                     mWindow;
    std::vector<Polyline>          mPolylines;
    std::vector<Marker>            mMarkers;
    size_t                         mNextLine{};
};

} // namespace playback::editor::ui
