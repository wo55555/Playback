#include "CameraTimelineRegistry.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace playback::keyframe {

namespace {

struct Binding {
    CameraTimelineHandle timeline;
};

std::atomic<std::shared_ptr<Binding const>>             gPreview;
std::atomic<std::shared_ptr<Binding const>>             gExport;
std::atomic<CameraTimelineRenderContextHandle>          gPreviewRenderContext;
std::atomic<CameraTimelineRenderContextHandle>          gExportRenderContext;
thread_local CameraTimelineRenderContextHandle          gRenderContext;

std::atomic<std::shared_ptr<Binding const>>& bindingFor(CameraTimelineSource source) {
    return source == CameraTimelineSource::Export ? gExport : gPreview;
}

std::atomic<CameraTimelineRenderContextHandle>& renderContextFor(CameraTimelineSource source) {
    return source == CameraTimelineSource::Export ? gExportRenderContext : gPreviewRenderContext;
}

} // namespace

ScopedCameraTimelineRenderContext::ScopedCameraTimelineRenderContext(
    CameraTimelineRenderContextHandle context
) noexcept
: mPrevious(std::move(gRenderContext)) {
    gRenderContext = std::move(context);
}

ScopedCameraTimelineRenderContext::~ScopedCameraTimelineRenderContext() {
    gRenderContext = std::move(mPrevious);
}

void publishCameraTimeline(CameraTimelineSource source, CameraTimelineHandle timeline) {
    if (!timeline) {
        clearCameraTimeline(source);
        return;
    }
    bindingFor(source).store(
        std::make_shared<Binding const>(Binding{std::move(timeline)}),
        std::memory_order_release
    );
}

void clearCameraTimeline(CameraTimelineSource source, CameraTimelineHandle const& expected) {
    auto& target  = bindingFor(source);
    auto  current = target.load(std::memory_order_acquire);
    while (current && (!expected || current->timeline == expected)) {
        if (target.compare_exchange_weak(current, {}, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (source == CameraTimelineSource::Preview) {
                clearCameraTimelineRenderContext(CameraTimelineSource::Preview);
            }
            return;
        }
    }
    if (!current && source == CameraTimelineSource::Preview && !expected) {
        clearCameraTimelineRenderContext(CameraTimelineSource::Preview);
    }
}

std::optional<CameraTimelineSample> sampleCameraTimeline(
    CameraTimelineSource                       source,
    visuals::ReplaySampleTime const& time,
    std::string_view                           cameraId
) noexcept {
    try {
        auto const current = bindingFor(source).load(std::memory_order_acquire);
        if (!current || !current->timeline) return std::nullopt;
        auto const evaluation =
            cameraId.empty() ? current->timeline->sample(time) : current->timeline->sampleCameraById(cameraId, time);
        if (!evaluation) return std::nullopt;
        return CameraTimelineSample{evaluation->state, evaluation->cameraId};
    } catch (...) {
        return std::nullopt;
    }
}

bool hasCameraTimeline(CameraTimelineSource source) noexcept {
    auto const current = bindingFor(source).load(std::memory_order_acquire);
    return current && current->timeline;
}

CameraTimelineRenderContextHandle publishCameraTimelineRenderContext(CameraTimelineRenderContext context) {
    auto handle = std::make_shared<CameraTimelineRenderContext const>(std::move(context));
    renderContextFor(handle->source).store(handle, std::memory_order_release);
    return handle;
}

void clearCameraTimelineRenderContext(
    CameraTimelineSource                     source,
    CameraTimelineRenderContextHandle const& expected
) noexcept {
    auto& target = renderContextFor(source);
    if (!expected) {
        target.store({}, std::memory_order_release);
        return;
    }

    auto current = target.load(std::memory_order_acquire);
    while (current == expected
           && !target.compare_exchange_weak(current, {}, std::memory_order_acq_rel, std::memory_order_acquire)) {}
}

CameraTimelineRenderContextHandle currentCameraTimelineRenderContext() noexcept {
    if (gRenderContext) return gRenderContext;
    if (auto context = gExportRenderContext.load(std::memory_order_acquire)) return context;
    return gPreviewRenderContext.load(std::memory_order_acquire);
}

} // namespace playback::keyframe
