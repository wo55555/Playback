#include "CameraPathOverlay.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/visuals/ReplaySampleTime.h"

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numbers>
#include <utility>

namespace playback::editor::ui {

namespace {

constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
constexpr float kInactiveOpacity  = 0.6f;
constexpr float kPathThickness    = 2.0f;
constexpr float kGlyphThickness   = 2.0f;
// The window holds at most three segments, so this budget keeps playback rebuilds inside a single frame.
constexpr int64_t kMaxSegmentSteps   = 512;
constexpr size_t  kMaxSamplesPerCall = 2048;

bool finite(::glm::vec3 const& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool finite(::glm::dvec4 const& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) && std::isfinite(point.w);
}

bool finite(keyframe::CameraRenderState const& pose) {
    return std::isfinite(pose.x) && std::isfinite(pose.y) && std::isfinite(pose.z) && std::isfinite(pose.yaw)
        && std::isfinite(pose.pitch) && std::isfinite(pose.roll) && std::isfinite(pose.fov);
}

ImU32 fade(ImU32 colour, float opacity) {
    auto const base  = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF);
    auto const alpha = static_cast<ImU32>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * base));
    return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

// Near plane first: a negative w flips the sign of the side distances. nearClipped means the start moved.
bool clipLine(::glm::dvec4& from, ::glm::dvec4& to, double nearW, double& first, double& last, bool& nearClipped) {
    first       = 0.0;
    last        = 1.0;
    nearClipped = false;
    if (!finite(from) || !finite(to)) return false;

    double const nearStart = from.w - nearW;
    double const nearEnd   = to.w - nearW;
    if (nearStart < 0.0 && nearEnd < 0.0) return false;
    if (nearStart < 0.0) {
        first       = nearStart / (nearStart - nearEnd);
        nearClipped = true;
    } else if (nearEnd < 0.0) {
        last = nearStart / (nearStart - nearEnd);
    }

    auto const delta   = to - from;
    auto const nearMin = from + delta * first;
    auto const nearMax = from + delta * last;

    std::array<double, 4> const start{
        nearMin.w + nearMin.x,
        nearMin.w - nearMin.x,
        nearMin.w + nearMin.y,
        nearMin.w - nearMin.y,
    };
    std::array<double, 4> const end{
        nearMax.w + nearMax.x,
        nearMax.w - nearMax.x,
        nearMax.w + nearMax.y,
        nearMax.w - nearMax.y,
    };
    double sideFirst = 0.0;
    double sideLast  = 1.0;
    for (size_t plane = 0; plane < start.size(); ++plane) {
        if (start[plane] < 0.0 && end[plane] < 0.0) return false;
        if (start[plane] < 0.0) {
            sideFirst = std::max(sideFirst, start[plane] / (start[plane] - end[plane]));
        } else if (end[plane] < 0.0) {
            sideLast = std::min(sideLast, start[plane] / (start[plane] - end[plane]));
        }
        if (sideFirst > sideLast) return false;
    }

    auto const nearDelta = nearMax - nearMin;
    from                 = nearMin + nearDelta * sideFirst;
    to                   = nearMin + nearDelta * sideLast;
    // Extreme endpoint magnitudes can round an intersection's w back to zero.
    from.w = std::max(from.w, nearW);
    to.w   = std::max(to.w, nearW);
    return finite(from) && finite(to);
}

ImVec2 screenPoint(::glm::dvec4 const& point, Rect const& rect) {
    return {
        rect.min.x + static_cast<float>(std::clamp(point.x / point.w, -1.0, 1.0) * 0.5 + 0.5) * rect.GetWidth(),
        rect.min.y + static_cast<float>(0.5 - std::clamp(point.y / point.w, -1.0, 1.0) * 0.5) * rect.GetHeight(),
    };
}

float distanceSquared(ImVec2 const& left, ImVec2 const& right) {
    float const dx = left.x - right.x;
    float const dy = left.y - right.y;
    return dx * dx + dy * dy;
}

struct Basis {
    ::glm::vec3 right;
    ::glm::vec3 up;
    ::glm::vec3 forward;
};

// Same convention as the render hooks: yaw 0 looks down +Z, positive pitch looks down.
Basis basisOf(keyframe::CameraRenderState const& pose) {
    float const yaw      = pose.yaw * kRadiansPerDegree;
    float const pitch    = pose.pitch * kRadiansPerDegree;
    float const sinYaw   = std::sin(yaw);
    float const cosYaw   = std::cos(yaw);
    float const sinPitch = std::sin(pitch);
    float const cosPitch = std::cos(pitch);
    Basis       basis{
        {-cosYaw,            0.0f,      -sinYaw          },
        {-sinYaw * sinPitch, cosPitch,  cosYaw * sinPitch},
        {-sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch},
    };
    float const roll = pose.roll * kRadiansPerDegree;
    if (std::abs(roll) > std::numeric_limits<float>::epsilon()) {
        float const cosine = std::cos(roll);
        float const sine   = std::sin(roll);
        auto const  right  = basis.right;
        auto const  up     = basis.up;
        basis.right        = right * cosine + up * sine;
        basis.up           = up * cosine - right * sine;
    }
    return {::glm::normalize(basis.right), ::glm::normalize(basis.up), ::glm::normalize(basis.forward)};
}

// World segments to screen polylines.
class PathPainter {
public:
    PathPainter(ImDrawList* drawList, ::glm::dmat4 const& transform, double nearW, Rect const& rect)
    : mDrawList(drawList),
      mTransform(transform),
      mNearW(nearW),
      mRect(rect) {}

    void style(ImU32 colour, float thickness) {
        flush();
        mColour    = colour;
        mThickness = thickness;
    }

    void segment(::glm::vec3 const& a, ::glm::vec3 const& b) {
        if (!finite(a) || !finite(b)) {
            flush();
            return;
        }
        auto   from = project(a);
        auto   to   = project(b);
        double first{};
        double last{};
        bool   nearClipped{};
        if (!clipLine(from, to, mNearW, first, last, nearClipped)) {
            flush();
            return;
        }
        auto const start = screenPoint(from, mRect);
        auto const end   = screenPoint(to, mRect);
        // Side clipping keeps the run joined; only re-entering through the near plane starts a new one.
        if (mPoints.empty() || nearClipped) {
            flush();
            mPoints.push_back(start);
        } else if (distanceSquared(mPoints.back(), start) > 0.25f) {
            mPoints.push_back(start);
        }
        if (distanceSquared(mPoints.back(), end) >= 0.01f) mPoints.push_back(end);
    }

    void flush() {
        if (mPoints.size() >= 2) {
            mDrawList
                ->AddPolyline(mPoints.data(), static_cast<int>(mPoints.size()), mColour, ImDrawFlags_None, mThickness);
        }
        mPoints.clear();
    }

private:
    [[nodiscard]] ::glm::dvec4 project(::glm::vec3 const& point) const {
        return mTransform * ::glm::dvec4{point.x, point.y, point.z, 1.0};
    }

    ImDrawList*         mDrawList;
    ::glm::dmat4        mTransform;
    double              mNearW;
    Rect                mRect;
    ImU32               mColour{};
    float               mThickness{1.0f};
    std::vector<ImVec2> mPoints;
};

void drawCameraGlyph(
    PathPainter&                       painter,
    keyframe::CameraRenderState const& pose,
    float                              aspect,
    ImU32                              colour,
    float                              thickness
) {
    if (!finite(pose)) return;
    float const fov        = std::clamp(pose.fov, 5.0f, 170.0f);
    float const focal      = 1.0f / std::tan(fov * 0.5f * kRadiansPerDegree);
    float const targetArea = std::sqrt(aspect) * 0.3f;
    float const depth      = (focal * std::sqrt(targetArea / aspect) + 0.5f) * 0.5f;
    float const halfH      = depth / focal;
    float const halfW      = halfH * aspect;

    auto const basis  = basisOf(pose);
    auto const apex   = ::glm::vec3{pose.x, pose.y, pose.z};
    auto const centre = apex + basis.forward * depth;
    auto const corner = [&](float sx, float sy) {
        return centre + basis.right * (sx * halfW) + basis.up * (sy * halfH);
    };
    auto const tl = corner(-1.0f, 1.0f);
    auto const tr = corner(1.0f, 1.0f);
    auto const br = corner(1.0f, -1.0f);
    auto const bl = corner(-1.0f, -1.0f);

    painter.style(colour, thickness);
    for (auto const& c : {tl, tr, br, bl}) {
        painter.segment(apex, c);
        painter.flush();
    }
    painter.segment(tl, tr);
    painter.segment(tr, br);
    painter.segment(br, bl);
    painter.segment(bl, tl);
    painter.flush();
}

struct NeighborTicks {
    int lastLast{-1};
    int last{-1};
    int next{-1};
    int nextNext{-1};
};

NeighborTicks neighborTicks(keyframe::CameraTimelineEvaluator const& timeline, int currentTick) {
    NeighborTicks window;
    // Segments are monotonic in tick, so rejecting the nearest out-of-segment key rejects every further one.
    auto const segment   = timeline.dimensionSegmentForTick(currentTick);
    auto const inSegment = [&](int tick) { return timeline.dimensionSegmentForTick(tick) == segment; };
    bool       hasLast   = false;
    bool       hasNext   = false;
    for (auto const& camera : timeline.cameras()) {
        if (!camera.enabled || camera.keysByTick.empty()) continue;
        auto const nextIt = camera.keysByTick.upper_bound(currentTick);
        if (nextIt != camera.keysByTick.begin()) {
            int const tick = std::prev(nextIt)->first;
            if (tick >= 0 && inSegment(tick) && (!hasLast || tick > window.last)) {
                window.last = tick;
                hasLast     = true;
            }
        }
        if (nextIt != camera.keysByTick.end() && nextIt->first >= 0 && inSegment(nextIt->first)) {
            if (!hasNext || nextIt->first < window.next) {
                window.next = nextIt->first;
                hasNext     = true;
            }
        }
    }
    if (!hasLast) window.last = -1;
    if (!hasNext) window.next = -1;

    bool hasLastLast = false;
    bool hasNextNext = false;
    for (auto const& camera : timeline.cameras()) {
        if (!camera.enabled || camera.keysByTick.empty()) continue;
        if (hasLast) {
            auto const it = camera.keysByTick.lower_bound(window.last);
            if (it != camera.keysByTick.begin()) {
                int const tick = std::prev(it)->first;
                if (tick >= 0 && inSegment(tick) && (!hasLastLast || tick > window.lastLast)) {
                    window.lastLast = tick;
                    hasLastLast     = true;
                }
            }
        }
        if (hasNext) {
            auto const it = camera.keysByTick.upper_bound(window.next);
            if (it != camera.keysByTick.end() && it->first >= 0 && inSegment(it->first)) {
                if (!hasNextNext || it->first < window.nextNext) {
                    window.nextNext = it->first;
                    hasNextNext     = true;
                }
            }
        }
    }
    if (!hasLastLast) window.lastLast = -1;
    if (!hasNextNext) window.nextNext = -1;
    return window;
}

bool holdAt(keyframe::CameraTimelineEvaluator const& timeline, int tick) {
    for (auto const& camera : timeline.cameras()) {
        if (!camera.enabled) continue;
        auto const it = camera.keysByTick.find(tick);
        if (it != camera.keysByTick.end()) {
            return it->second.interpolationType == state::editing::model::CameraInterpolationType::Hold;
        }
    }
    return false;
}

} // namespace

void CameraPathOverlay::clear() {
    mPolylines.clear();
    mMarkers.clear();
    mPreviousPolylines.clear();
    mPreviousMarkers.clear();
    mTimeline.reset();
    mWindow   = {};
    mNextLine = 0;
}

void CameraPathOverlay::rebuild(keyframe::CameraTimelineHandle const& timeline, PathWindow const& window) {
    bool const sameTimeline = mTimeline == timeline;
    if (sameTimeline && !isBuilding()) {
        mPreviousPolylines = std::move(mPolylines);
        mPreviousMarkers   = std::move(mMarkers);
    } else if (!sameTimeline) {
        mPreviousPolylines.clear();
        mPreviousMarkers.clear();
    }
    mPolylines.clear();
    mMarkers.clear();
    mNextLine = 0;
    mTimeline = timeline;
    mWindow   = window;
    if (!timeline) return;

    auto addMarker = [&](int tick, float opacity) {
        if (tick < 0) return;
        auto const sample = timeline->sample({tick, 1});
        if (!sample || !finite(sample->state)) return;
        mMarkers.push_back(
            Marker{
                tick,
                timeline->dimensionSegmentForTick(tick),
                sample->state,
                opacity,
            }
        );
    };
    auto addSegment = [&](int from, int to, float opacity) {
        if (from < 0 || to < 0 || to <= from) return;
        if (holdAt(*timeline, from)) return;
        auto const dimension = timeline->dimensionSegmentForTick(from);
        if (timeline->dimensionSegmentForTick(to) != dimension) return;
        int64_t const duration = static_cast<int64_t>(to) - from;
        int64_t const steps    = std::min<int64_t>(kMaxSegmentSteps, std::max<int64_t>(8, duration));
        Polyline      line{dimension, {}, from, to, steps, opacity};
        line.points.reserve(static_cast<size_t>(steps + 1));
        mPolylines.push_back(std::move(line));
    };

    addMarker(window.last, 1.0f);
    if (window.lastLast >= 0) {
        addMarker(window.lastLast, kInactiveOpacity);
        addSegment(window.lastLast, window.last, kInactiveOpacity);
    }
    addMarker(window.next, 1.0f);
    if (window.nextNext >= 0) {
        addMarker(window.nextNext, kInactiveOpacity);
        addSegment(window.next, window.nextNext, kInactiveOpacity);
    }
    addSegment(window.last, window.next, 1.0f);
}

void CameraPathOverlay::advanceBuild() {
    if (!mTimeline) return;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
    size_t     sampled  = 0;
    while (mNextLine < mPolylines.size()) {
        auto&      line  = mPolylines[mNextLine];
        auto const index = static_cast<int64_t>(line.points.size());
        if (index > line.steps) {
            ++mNextLine;
            if (!isBuilding()) {
                mPreviousPolylines.clear();
                mPreviousMarkers.clear();
            }
            continue;
        }
        int64_t const duration = static_cast<int64_t>(line.toTick) - line.fromTick;
        auto const    sample =
            mTimeline->sample({static_cast<int64_t>(line.fromTick) * line.steps + duration * index, line.steps});
        line.points.push_back(
            sample ? ::glm::vec3{sample->state.x, sample->state.y, sample->state.z}
                   : ::glm::vec3{std::numeric_limits<float>::quiet_NaN()}
        );
        if (++sampled >= kMaxSamplesPerCall || std::chrono::steady_clock::now() >= deadline) return;
    }
}

void CameraPathOverlay::draw(PanelContext const& ctx, Rect const& videoRect, ImDrawList* drawList) {
    auto const& timeline = ctx.state.cameraTimeline;
    if (!ctx.state.editorVisible || !ctx.project() || !timeline || !ctx.commands.isCameraPathVisible()) {
        clear();
        return;
    }
    if (!ctx.state.hudVisible || !ctx.cameraProjection || exporting::isExportActive(ctx.state.exportStatus.state)
        || videoRect.GetWidth() <= 0.0f || videoRect.GetHeight() <= 0.0f) {
        return;
    }
    auto const       ticks = neighborTicks(*timeline, ctx.state.currentTick);
    PathWindow const window{ticks.lastLast, ticks.last, ticks.next, ticks.nextNext};
    if (mTimeline != timeline || mWindow != window) rebuild(timeline, window);
    advanceBuild();

    auto const   dimension = timeline->dimensionSegmentForTick(ctx.state.currentTick);
    double const nearW     = std::max(0.001, static_cast<double>(ctx.cameraProjection->nearClipW));
    float const  aspect    = videoRect.GetWidth() / videoRect.GetHeight();
    PathPainter  painter(drawList, ctx.cameraProjection->viewProjection, nearW, videoRect);

    bool const  building = isBuilding() && !mPreviousPolylines.empty();
    auto const& lines    = building ? mPreviousPolylines : mPolylines;
    auto const& markers  = building ? mPreviousMarkers : mMarkers;

    drawList->PushClipRect(videoRect.min, videoRect.max, true);
    for (auto const& line : lines) {
        if (line.dimensionSegment != dimension || line.points.size() < 2) continue;
        painter.style(fade(theme::kCameraPath, line.opacity), kPathThickness);
        for (size_t index = 1; index < line.points.size(); ++index) {
            painter.segment(line.points[index - 1], line.points[index]);
        }
        painter.flush();
    }
    for (auto const& marker : markers) {
        if (marker.dimensionSegment != dimension) continue;
        drawCameraGlyph(painter, marker.pose, aspect, fade(theme::kCameraPath, marker.opacity), kGlyphThickness);
    }
    if (ctx.state.paused) {
        // Reuse the render frame's sample time so the glyph matches the pose the camera was drawn with.
        auto const time =
            ctx.cameraProjection->sampleTime.value_or(visuals::ReplaySampleTime{ctx.state.currentTick, 1});
        if (auto const current = timeline->sample(time)) {
            drawCameraGlyph(painter, current->state, aspect, theme::kCameraPathPlayhead, kGlyphThickness);
        }
    }
    drawList->PopClipRect();
}

} // namespace playback::editor::ui
