#pragma once

#include "playback/editor/editing/models/CameraKeyframe.h"
#include "playback/editor/ui/components/Splitter.h"

#include "imgui.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::editor::ui {

struct BezierPoint {
    float                t{0};            // x ∈ [0,1]
    float                v{0};            // y
    editing::model::Vec2 inTangent{0, 0}; // relative control point offset
    editing::model::Vec2 outTangent{0, 0};
};

struct BezierCurve {
    std::string              name;
    std::vector<BezierPoint> points;
    bool                     loop{false};

    // Sample y at t (0..1)
    float sample(float t) const;
};

class BezierCurveEditor {
public:
    void setCurve(const BezierCurve& curve);
    void setSampleRange(float tMin, float tMax);

    // UI
    void draw(ImDrawList* dl, Rect area);
    // Output
    [[nodiscard]] BezierCurve curve() const;
    float                     sampleAt(float t) const;

private:
    // Locate the segment containing t
    struct Segment {
        int lo, hi;
    };
    [[nodiscard]] std::optional<Segment> locateSegment(const std::vector<BezierPoint>& pts, float t) const;

    // Newton-Raphson: find y from x on a Bezier segment
    float bezierYFromX(float u, const BezierPoint& a, const BezierPoint& b) const;

    BezierCurve mCurve;
    float       mTMin{0.0f};
    float       mTMax{1.0f};
};

} // namespace playback::editor::ui
