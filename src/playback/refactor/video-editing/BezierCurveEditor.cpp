#include "BezierCurveEditor.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace playback::refactor::video_editing {

// ===== BezierCurve::sample =====

float BezierCurve::sample(float t) const {
    if (points.size() < 2) return t;
    t = std::clamp(t, 0.0f, 1.0f);

    // Locate segment
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (t >= points[i].t && t <= points[i + 1].t) {
            float u = (t - points[i].t) / (points[i + 1].t - points[i].t);

            // De Casteljau for Cubic Bezier
            if (i + 1 < points.size()) {
                Vec2 p0{points[i].t, points[i].v};
                Vec2 p1{points[i].t + points[i].outTangent.x, points[i].v + points[i].outTangent.y};
                Vec2 p2{points[i + 1].t + points[i + 1].inTangent.x, points[i + 1].v + points[i + 1].inTangent.y};
                Vec2 p3{points[i + 1].t, points[i + 1].v};

                // De Casteljau: recurse
                auto lerp = [](const Vec2& a, const Vec2& b, float t) -> Vec2 {
                    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                };

                Vec2 q0 = lerp(p0, p1, u);
                Vec2 q1 = lerp(p1, p2, u);
                Vec2 q2 = lerp(p2, p3, u);
                Vec2 r0 = lerp(q0, q1, u);
                Vec2 r1 = lerp(q1, q2, u);
                Vec2 s  = lerp(r0, r1, u);

                return s.y;
            }

            // Linear fallback
            return points[i].v + (points[i + 1].v - points[i].v) * u;
        }
    }

    return t;
}

// ===== BezierCurveEditor =====

void BezierCurveEditor::setCurve(const BezierCurve& curve) {
    mCurve = curve;
}

void BezierCurveEditor::setSampleRange(float tMin, float tMax) {
    mTMin = tMin;
    mTMax = tMax;
}

BezierCurve BezierCurveEditor::curve() const {
    return mCurve;
}

float BezierCurveEditor::sampleAt(float t) const {
    return mCurve.sample(t);
}

void BezierCurveEditor::draw(ImDrawList* dl, Rect area) {
    // Background
    dl->AddRectFilled(area.min, area.max, IM_COL32(0x1a, 0x1a, 0x1a, 0xff));
    dl->AddRect(area.min, area.max, IM_COL32(0x3a, 0x3a, 0x3a, 0xff));

    // Grid lines (4x4)
    for (int i = 1; i < 4; ++i) {
        float x = area.min.x + area.GetWidth() * (i / 4.0f);
        float y = area.min.y + area.GetHeight() * (i / 4.0f);
        dl->AddLine(ImVec2(x, area.min.y), ImVec2(x, area.max.y), IM_COL32(0x2a, 0x2a, 0x2a, 0xff));
        dl->AddLine(ImVec2(area.min.x, y), ImVec2(area.max.x, y), IM_COL32(0x2a, 0x2a, 0x2a, 0xff));
    }

    if (mCurve.points.empty()) return;

    // Draw curve (64 segments)
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 64; ++i) {
        float t = static_cast<float>(i) / 64.0f;
        float y = sampleAt(t);
        pts.push_back({
            area.min.x + area.GetWidth() * t,
            area.max.y - area.GetHeight() * y
        });
    }

    for (size_t i = 1; i < pts.size(); ++i) {
        dl->AddLine(pts[i - 1], pts[i], IM_COL32(0xf0, 0xc0, 0x20, 0xff), 2.0f);
    }

    // Draw control points
    for (const auto& p : mCurve.points) {
        ImVec2 cp{
            area.min.x + area.GetWidth() * p.t,
            area.max.y - area.GetHeight() * p.v
        };
        dl->AddCircleFilled(cp, 5.0f, IM_COL32(0xf0, 0x80, 0x20, 0xff));
        dl->AddCircle(cp, 7.0f, IM_COL32(0xff, 0xff, 0xff, 0x80), 0, 1.0f);
    }
}

void BezierCurveEditor::handleInput(const ImGuiIO& io, Rect area) {
    // Placeholder: drag control points
    // Will be implemented with per-point hit testing + drag delta
}

std::optional<BezierCurveEditor::Segment> BezierCurveEditor::locateSegment(
    const std::vector<BezierPoint>& pts, float t) const
{
    if (pts.size() < 2) return std::nullopt;

    if (t <= pts.front().t) return Segment{0, 1};
    if (t >= pts.back().t) return Segment{static_cast<int>(pts.size()) - 2, static_cast<int>(pts.size()) - 1};

    for (int i = 0; i + 1 < static_cast<int>(pts.size()); ++i) {
        if (t >= pts[i].t && t <= pts[i + 1].t) {
            return Segment{i, i + 1};
        }
    }

    return std::nullopt;
}

float BezierCurveEditor::bezierYFromX(float u, const BezierPoint& a, const BezierPoint& b) const {
    // Simplified: lerp between a.v and b.v
    // Full implementation would use Newton-Raphson on the cubic Bezier
    return a.v + (b.v - a.v) * u;
}

} // namespace playback::refactor::video_editing