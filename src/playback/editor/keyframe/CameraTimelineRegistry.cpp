#include "CameraTimelineRegistry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace playback::editor::keyframe {

namespace {

struct Binding {
    CameraTimelineHandle timeline;
    std::optional<float> aspectRatio;
};

std::atomic<std::shared_ptr<Binding const>> gPreview;
std::atomic<std::shared_ptr<Binding const>> gExport;
std::atomic<std::shared_ptr<CameraRenderState const>> gPreviewOverride;
std::atomic<CameraTimelineRenderContextHandle> gPreviewRenderContext;
std::atomic<CameraTimelineRenderContextHandle> gExportRenderContext;
thread_local std::optional<CameraTimelineRenderContext> gRenderContext;

std::atomic<std::shared_ptr<Binding const>>& bindingFor(CameraTimelineSource source) {
    return source == CameraTimelineSource::Export ? gExport : gPreview;
}

} // namespace

ScopedCameraTimelineRenderContext::ScopedCameraTimelineRenderContext(
    functions::render::ReplaySampleTime time,
    CameraTimelineSource                source,
    std::optional<CameraTimelineSample> sample,
    CameraTimelineAppliedFlag           appliedFlag
) noexcept
: mPrevious(gRenderContext),
  mHadPrevious(gRenderContext.has_value()) {
    gRenderContext = CameraTimelineRenderContext{time, source, std::move(sample), std::move(appliedFlag)};
}

ScopedCameraTimelineRenderContext::~ScopedCameraTimelineRenderContext() {
    if (mHadPrevious) gRenderContext = mPrevious;
    else gRenderContext.reset();
}

void publishCameraTimeline(
    CameraTimelineSource source,
    CameraTimelineHandle timeline,
    std::optional<float> aspectRatio
) {
    if (!timeline) {
        clearCameraTimeline(source);
        return;
    }
    if (!aspectRatio || !std::isfinite(*aspectRatio) || *aspectRatio <= 0.0f) aspectRatio.reset();
    bindingFor(source).store(
        std::make_shared<Binding const>(Binding{std::move(timeline), aspectRatio}),
        std::memory_order_release
    );
}

void clearCameraTimeline(CameraTimelineSource source, CameraTimelineHandle const& expected) {
    auto& target  = bindingFor(source);
    auto  current = target.load(std::memory_order_acquire);
    while (current && (!expected || current->timeline == expected)) {
        if (target.compare_exchange_weak(current, {}, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (source == CameraTimelineSource::Preview) clearPreviewCameraOverride();
            return;
        }
    }
    if (!current && source == CameraTimelineSource::Preview && !expected) clearPreviewCameraOverride();
}

std::optional<CameraTimelineSample>
sampleCameraTimeline(CameraTimelineSource source, functions::render::ReplaySampleTime const& time) noexcept {
    try {
        auto const current = bindingFor(source).load(std::memory_order_acquire);
        if (!current || !current->timeline) return std::nullopt;
        auto const state = current->timeline->sample(time);
        if (!state) return std::nullopt;
        return CameraTimelineSample{*state, current->aspectRatio};
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<CameraRenderState> sampleCameraTimelineRange(
    CameraTimelineSource source,
    int64_t              startTick,
    int64_t              endTick,
    size_t               maxSamples
) noexcept {
    std::vector<CameraRenderState> result;
    if (startTick < 0 || endTick < startTick || maxSamples == 0) return result;

    try {
        auto const current = bindingFor(source).load(std::memory_order_acquire);
        if (!current || !current->timeline) return result;

        auto const span = endTick - startTick;
        if (span == 0 || maxSamples == 1) {
            auto const state = current->timeline->sample({startTick, 1});
            if (state) result.push_back(*state);
            return result;
        }

        auto const denominator = static_cast<int64_t>(std::max<size_t>(1, maxSamples - 1));
        auto const step        = std::max<int64_t>(1, (span + denominator - 1) / denominator);
        result.reserve(static_cast<size_t>(span / step) + 2);
        for (int64_t tick = startTick; tick <= endTick;) {
            auto const state = current->timeline->sample({tick, 1});
            if (state) result.push_back(*state);
            if (tick > endTick - step) break;
            tick += step;
        }
        if ((endTick - startTick) % step != 0) {
            auto const state = current->timeline->sample({endTick, 1});
            if (state) result.push_back(*state);
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

bool hasCameraTimeline(CameraTimelineSource source) noexcept {
    auto const current = bindingFor(source).load(std::memory_order_acquire);
    return current && current->timeline;
}

void publishPreviewCameraOverride(CameraRenderState state) noexcept {
    if (!std::isfinite(state.x) || !std::isfinite(state.y) || !std::isfinite(state.z)
        || !std::isfinite(state.yaw) || !std::isfinite(state.pitch) || !std::isfinite(state.roll)
        || !std::isfinite(state.fov)) {
        return;
    }
    state.pitch = std::clamp(state.pitch, -89.9f, 89.9f);
    state.fov   = std::clamp(state.fov, 1.0f, 179.0f);
    gPreviewOverride.store(std::make_shared<CameraRenderState const>(state), std::memory_order_release);
}

void clearPreviewCameraOverride() noexcept { gPreviewOverride.store({}, std::memory_order_release); }

std::optional<CameraRenderState> currentPreviewCameraOverride() noexcept {
    auto const state = gPreviewOverride.load(std::memory_order_acquire);
    return state ? std::optional<CameraRenderState>{*state} : std::nullopt;
}

std::optional<CameraTimelineRenderContext> currentCameraTimelineRenderContext() noexcept {
    if (gRenderContext) return gRenderContext;
    auto const exportContext = gExportRenderContext.load(std::memory_order_acquire);
    if (exportContext) return std::optional<CameraTimelineRenderContext>{*exportContext};
    auto const previewContext = gPreviewRenderContext.load(std::memory_order_acquire);
    return previewContext ? std::optional<CameraTimelineRenderContext>{*previewContext} : std::nullopt;
}

void publishCameraTimelineRenderContext(CameraTimelineRenderContextHandle context) {
    if (!context) return;
    auto& target = context->source == CameraTimelineSource::Export ? gExportRenderContext : gPreviewRenderContext;
    target.store(std::move(context), std::memory_order_release);
}

void clearCameraTimelineRenderContext(CameraTimelineRenderContextHandle const& expected) {
    auto clear = [&](std::atomic<CameraTimelineRenderContextHandle>& target) {
        auto current = target.load(std::memory_order_acquire);
        while (current && (!expected || current == expected)) {
            if (target.compare_exchange_weak(
                    current,
                    {},
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                )) {
                return;
            }
        }
    };
    if (expected) {
        clear(expected->source == CameraTimelineSource::Export ? gExportRenderContext : gPreviewRenderContext);
    } else {
        clear(gExportRenderContext);
        clear(gPreviewRenderContext);
    }
}

} // namespace playback::editor::keyframe
