#include "CameraPathOverlay.h"

#include "playback/editor/ui/EditorTheme.h"

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
#include <string_view>
#include <utility>

namespace playback::editor::ui {

namespace {

constexpr float   kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
constexpr float   kInactiveOpacity  = 0.6f;
constexpr float   kPathThickness    = 2.0f;
constexpr float   kGlyphThickness   = 2.0f;
constexpr int64_t kMaxSegmentSteps  = 2000;

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

// Clips against w >= nearW and the four side planes; first/last are the kept parameter range along from->to.
bool clipLine(::glm::dvec4& from, ::glm::dvec4& to, double nearW, double& first, double& last) {
    first = 0.0;
    last  = 1.0;
    if (!finite(from) || !finite(to)) return false;
    std::array<double, 5> const
        start{from.w - nearW, from.w + from.x, from.w - from.x, from.w + from.y, from.w - from.y};
    std::array<double, 5> const end{to.w - nearW, to.w + to.x, to.w - to.x, to.w + to.y, to.w - to.y};
    for (size_t plane = 0; plane < start.size(); ++plane) {
        if (start[plane] < 0.0 && end[plane] < 0.0) return false;
        if (start[plane] < 0.0) first = std::max(first, start[plane] / (start[plane] - end[plane]));
        else if (end[plane] < 0.0) last = std::min(last, start[plane] / (start[plane] - end[plane]));
        if (first > last) return false;
    }
    auto const delta = to - from;
    to               = from + delta * last;
    from             = from + delta * first;
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

// Joins consecutive world-space segments into anti-aliased screen polylines.
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
        if (!clipLine(from, to, mNearW, first, last)) {
            flush();
            return;
        }
        auto const start = screenPoint(from, mRect);
        auto const end   = screenPoint(to, mRect);
        // A clipped start means the run re-enters the view, so it must not join the previous point.
        if (mPoints.empty() || first > 0.0 || distanceSquared(mPoints.back(), start) > 0.25f) {
            flush();
            mPoints.push_back(start);
        }
        if (distanceSquared(mPoints.back(), end) >= 0.01f) mPoints.push_back(end);
        if (last < 1.0) flush();
    }

    // Screen position of a world point strictly inside the frustum, so callers can size glyphs on screen.
    [[nodiscard]] bool visiblePoint(::glm::vec3 const& point, ImVec2& out) const {
        if (!finite(point)) return false;
        auto const clip = project(point);
        if (!finite(clip) || clip.w < mNearW || std::abs(clip.x) > clip.w || std::abs(clip.y) > clip.w) return false;
        out = screenPoint(clip, mRect);
        return true;
    }

    void dot(ImVec2 const& centre, ImU32 colour, float radius) {
        flush();
        mDrawList->AddCircleFilled(centre, radius, colour, 12);
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
    float const fov = std::clamp(pose.fov, 5.0f, 170.0f);
    // Focal-length based sizing keeps the glyph a similar on-screen size regardless of fov.
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

    // Distant glyphs collapse to sub-pixel strokes that the painter drops; a line-width dot keeps the keyframe visible.
    ImVec2 apexScreen{};
    ImVec2 cornerScreen{};
    if (painter.visiblePoint(apex, apexScreen) && painter.visiblePoint(tr, cornerScreen)
        && distanceSquared(apexScreen, cornerScreen) < 9.0f) {
        painter.dot(apexScreen, colour, thickness);
        return;
    }

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

} // namespace

void CameraPathOverlay::clear() {
    mPaths.clear();
    mTimeline.reset();
    mNextPath = 0;
    mNextLine = 0;
}

void CameraPathOverlay::rebuild(keyframe::CameraTimelineHandle const& timeline) {
    clear();
    for (auto const& camera : timeline->cameras()) {
        if (!camera.enabled || camera.keysByTick.empty()) continue;
        CameraPath  path{camera.id, {}, {}};
        auto const& keys = camera.keysByTick;
        // Up to one sample per tick and 2000 per segment; the total budget keeps huge tracks incremental.
        auto const resolution = std::clamp<int64_t>(
            65536 / static_cast<int64_t>(std::max<size_t>(1, keys.size() - 1)),
            32,
            kMaxSegmentSteps
        );
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            int const tick = it->first;
            if (tick < 0) continue;
            size_t const                      dimension = timeline->dimensionSegmentForTick(tick);
            auto const&                       key       = it->second;
            keyframe::CameraRenderState const pose{
                key.position.x,
                key.position.y,
                key.position.z,
                key.yaw,
                key.pitch,
                key.roll,
                key.fov,
            };
            if (finite(pose)) path.markers.push_back({tick, dimension, pose});

            auto const next = std::next(it);
            if (next == keys.end() || timeline->dimensionSegmentForTick(next->first) != dimension
                || key.interpolationType == state::editing::model::CameraInterpolationType::Hold) {
                continue;
            }
            int64_t const duration = static_cast<int64_t>(next->first) - tick;
            int64_t const steps    = std::min<int64_t>(resolution, std::max<int64_t>(8, duration));
            Polyline      line{dimension, {}, tick, next->first, steps};
            line.points.reserve(static_cast<size_t>(steps + 1));
            path.polylines.push_back(std::move(line));
        }
        mPaths.push_back(std::move(path));
    }
    mTimeline = timeline;
}

void CameraPathOverlay::advanceBuild() {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
    size_t     sampled  = 0;
    while (mNextPath < mPaths.size()) {
        auto& path = mPaths[mNextPath];
        if (mNextLine == path.polylines.size()) {
            ++mNextPath;
            mNextLine = 0;
            continue;
        }
        auto&      line  = path.polylines[mNextLine];
        auto const index = static_cast<int64_t>(line.points.size());
        if (index > line.steps) {
            ++mNextLine;
            continue;
        }
        int64_t const duration = static_cast<int64_t>(line.toTick) - line.fromTick;
        auto const    sample   = mTimeline->sampleCameraById(
            path.cameraId,
            {static_cast<int64_t>(line.fromTick) * line.steps + duration * index, line.steps}
        );
        line.points.push_back(
            sample ? ::glm::vec3{sample->state.x, sample->state.y, sample->state.z}
                   : ::glm::vec3{std::numeric_limits<float>::quiet_NaN()}
        );
        if (++sampled >= 512 || std::chrono::steady_clock::now() >= deadline) return;
    }
}

void CameraPathOverlay::draw(PanelContext const& ctx, Rect const& videoRect, ImDrawList* drawList) {
    auto const& timeline = ctx.state.cameraTimeline;
    if (!ctx.state.editorVisible || !ctx.project() || !timeline || !ctx.commands.isCameraPathVisible()) {
        clear();
        return;
    }
    if (!ctx.cameraProjection || exporting::isExportActive(ctx.state.exportStatus.state) || videoRect.GetWidth() <= 0.0f
        || videoRect.GetHeight() <= 0.0f) {
        return;
    }
    if (mTimeline != timeline) rebuild(timeline);
    advanceBuild();

    int const              currentTick    = ctx.state.currentTick;
    auto const             dimension      = timeline->dimensionSegmentForTick(currentTick);
    auto const*            selectedKey    = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    auto const*            selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    std::string_view const selectedId     = selectedKey    ? selectedKey->trackId
                                          : selectedCamera ? selectedCamera->cameraId
                                                           : std::string_view{};
    double const           nearW          = std::max(0.001, static_cast<double>(ctx.cameraProjection->nearClipW));
    float const            aspect         = videoRect.GetWidth() / videoRect.GetHeight();
    PathPainter            painter(drawList, ctx.cameraProjection->viewProjection, nearW, videoRect);

    drawList->PushClipRect(videoRect.min, videoRect.max, true);
    for (auto const& path : mPaths) {
        bool const  selected = path.cameraId == selectedId;
        ImU32 const colour   = selected ? theme::kAccent : theme::kCameraPath;

        // The segment around the playhead stays solid and its neighbours are dimmed.
        Polyline const* focus         = nullptr;
        int64_t         focusDistance = std::numeric_limits<int64_t>::max();
        for (auto const& line : path.polylines) {
            if (line.dimensionSegment != dimension) continue;
            int64_t const distance = currentTick < line.fromTick ? line.fromTick - currentTick
                                   : currentTick > line.toTick   ? currentTick - line.toTick
                                                                 : 0;
            if (distance < focusDistance) {
                focus         = &line;
                focusDistance = distance;
            }
        }

        for (auto const& line : path.polylines) {
            if (line.dimensionSegment != dimension || line.points.size() < 2) continue;
            painter.style(fade(colour, &line == focus ? 1.0f : kInactiveOpacity), kPathThickness);
            for (size_t index = 1; index < line.points.size(); ++index) {
                painter.segment(line.points[index - 1], line.points[index]);
            }
            painter.flush();
        }

        for (auto const& marker : path.markers) {
            if (marker.dimensionSegment != dimension) continue;
            bool const  keySelected = selectedKey && selected && selectedKey->tick == marker.tick;
            bool const  nearFocus   = !focus || (marker.tick >= focus->fromTick && marker.tick <= focus->toTick);
            ImU32 const glyphColour =
                keySelected ? theme::kCameraPathSelected : fade(colour, nearFocus ? 1.0f : kInactiveOpacity);
            drawCameraGlyph(painter, marker.pose, aspect, glyphColour, kGlyphThickness);
        }
    }
    if (ctx.state.paused) {
        if (auto const current = timeline->sample({currentTick, 1})) {
            drawCameraGlyph(painter, current->state, aspect, theme::kSuccess, kGlyphThickness);
        }
    }
    drawList->PopClipRect();
}

} // namespace playback::editor::ui
