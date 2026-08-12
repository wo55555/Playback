#pragma once

#include "CameraTimelineEvaluator.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace playback::editor::keyframe {

enum class CameraTimelineSource : uint8_t { Preview, Export };

using CameraTimelineHandle = std::shared_ptr<CameraTimelineEvaluator const>;

struct CameraTimelineSample {
    CameraRenderState state;
    std::optional<float> aspectRatio;
};

using CameraTimelineAppliedFlag = std::shared_ptr<std::atomic_bool>;

// A render pass captures one immutable timeline source and one exact replay
// sample.  Hooks consume this context without reaching into editor state.
struct CameraTimelineRenderContext {
    functions::render::ReplaySampleTime time;
    CameraTimelineSource                source{CameraTimelineSource::Preview};
    // Export assigns the offline clock token so an acknowledgement always
    // belongs to the exact frame being rendered.
    uint64_t                            renderToken{};
    // The producer computes this once before the native render pass so camera
    // and entity interpolation consume the same fractional replay sample.
    std::optional<CameraTimelineSample> sample;
    // The flag is shared with the export boundary so a renderer-thread camera
    // hook can acknowledge the same sample after updateGraphics returns.
    CameraTimelineAppliedFlag           appliedFlag;
};

using CameraTimelineRenderContextHandle = std::shared_ptr<CameraTimelineRenderContext const>;

class ScopedCameraTimelineRenderContext {
public:
    ScopedCameraTimelineRenderContext(
        functions::render::ReplaySampleTime time,
        CameraTimelineSource                source,
        std::optional<CameraTimelineSample> sample = std::nullopt,
        CameraTimelineAppliedFlag           appliedFlag = {},
        uint64_t                            renderToken = 0
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
sampleCameraTimeline(
    CameraTimelineSource                       source,
    functions::render::ReplaySampleTime const& time,
    std::string_view                           cameraId = {}
) noexcept;

// Samples one immutable evaluator handle across a bounded integer-tick range.
// The viewport uses this to visualize exactly the same path as preview/export.
[[nodiscard]] std::vector<CameraRenderState> sampleCameraTimelineRange(
    CameraTimelineSource source,
    int64_t              startTick,
    int64_t              endTick,
    size_t               maxSamples,
    std::string_view      cameraId = {}
) noexcept;

[[nodiscard]] std::optional<CameraPathSampleRange> sampleCameraTimelinePathAround(
    CameraTimelineSource                       source,
    functions::render::ReplaySampleTime const& time,
    size_t                                     maxSamples,
    std::string_view                           cameraId
) noexcept;

[[nodiscard]] bool hasCameraTimeline(CameraTimelineSource source) noexcept;

// A paused editor may temporarily inspect the timeline from an operator
// camera. This render-only state never participates in export evaluation.
void publishPreviewCameraOverride(CameraRenderState state) noexcept;
void clearPreviewCameraOverride() noexcept;
[[nodiscard]] std::optional<CameraRenderState> currentPreviewCameraOverride() noexcept;

[[nodiscard]] std::optional<CameraTimelineRenderContext> currentCameraTimelineRenderContext() noexcept;

// Export keeps its immutable render context alive independently of the game
// thread. This is required when LevelRendererPlayer runs on a renderer thread.
void publishCameraTimelineRenderContext(CameraTimelineRenderContextHandle context);
void clearCameraTimelineRenderContext(CameraTimelineRenderContextHandle const& expected = {});

} // namespace playback::editor::keyframe
