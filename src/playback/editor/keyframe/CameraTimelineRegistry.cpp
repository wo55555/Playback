#include "CameraTimelineRegistry.h"

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

std::atomic<std::shared_ptr<Binding const>>& bindingFor(CameraTimelineSource source) {
    return source == CameraTimelineSource::Export ? gExport : gPreview;
}

} // namespace

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
        if (target.compare_exchange_weak(current, {}, std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
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

} // namespace playback::editor::keyframe
