#pragma once

#include "CameraTimelineEvaluator.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace playback::editor::keyframe {

enum class CameraTimelineSource : uint8_t { Preview, Export };

using CameraTimelineHandle = std::shared_ptr<CameraTimelineEvaluator const>;

struct CameraTimelineSample {
    CameraRenderState state;
    std::optional<float> aspectRatio;
};

using CameraTimelineAppliedCallback = void (*)(void*) noexcept;

// A render pass captures one immutable timeline source and one exact replay
// sample.  Hooks consume this context without reaching into editor state.
struct CameraTimelineRenderContext {
    functions::render::ReplaySampleTime time;
    CameraTimelineSource                source{CameraTimelineSource::Preview};
    // Export computes this once before the native render pass. Preview may
    // leave it empty and let the camera hook sample the active timeline.
    std::optional<CameraTimelineSample> sample;
    CameraTimelineAppliedCallback       appliedCallback{};
    void*                               appliedContext{};
};

class ScopedCameraTimelineRenderContext {
public:
    ScopedCameraTimelineRenderContext(
        functions::render::ReplaySampleTime time,
        CameraTimelineSource                source,
        std::optional<CameraTimelineSample> sample = std::nullopt,
        CameraTimelineAppliedCallback       appliedCallback = nullptr,
        void*                               appliedContext = nullptr
    ) noexcept;
    ~ScopedCameraTimelineRenderContext();

    ScopedCameraTimelineRenderContext(ScopedCameraTimelineRenderContext const&)            = delete;
    ScopedCameraTimelineRenderContext& operator=(ScopedCameraTimelineRenderContext const&) = delete;

private:
    std::optional<CameraTimelineRenderContext> mPrevious;
    bool                                       mHadPrevious{};
};

void publishCameraTimeline(
    CameraTimelineSource source,
    CameraTimelineHandle timeline,
    std::optional<float> aspectRatio = std::nullopt
);
void clearCameraTimeline(CameraTimelineSource source, CameraTimelineHandle const& expected = {});

[[nodiscard]] std::optional<CameraTimelineSample>
sampleCameraTimeline(CameraTimelineSource source, functions::render::ReplaySampleTime const& time) noexcept;

[[nodiscard]] bool hasCameraTimeline(CameraTimelineSource source) noexcept;

[[nodiscard]] std::optional<CameraTimelineRenderContext> currentCameraTimelineRenderContext() noexcept;

} // namespace playback::editor::keyframe
