#pragma once

#include "CameraTimelineEvaluator.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace playback::editor::keyframe {

enum class CameraTimelineSource : uint8_t { Preview, Export };

using CameraTimelineHandle = std::shared_ptr<CameraTimelineEvaluator const>;

struct CameraTimelineSample {
    CameraRenderState state;
    std::string       cameraId;
};

using CameraTimelineAppliedFlag = std::shared_ptr<std::atomic_bool>;

struct CameraTimelineRenderContext {
    visuals::ReplaySampleTime time;
    CameraTimelineSource                source{CameraTimelineSource::Preview};
    uint64_t                            renderToken{};
    std::optional<CameraTimelineSample> sample;
    CameraTimelineAppliedFlag           appliedFlag;
    uint64_t                            frameIndex{};
};

using CameraTimelineRenderContextHandle = std::shared_ptr<CameraTimelineRenderContext const>;

class ScopedCameraTimelineRenderContext {
public:
    explicit ScopedCameraTimelineRenderContext(CameraTimelineRenderContextHandle context) noexcept;
    ~ScopedCameraTimelineRenderContext();

    ScopedCameraTimelineRenderContext(ScopedCameraTimelineRenderContext const&)            = delete;
    ScopedCameraTimelineRenderContext& operator=(ScopedCameraTimelineRenderContext const&) = delete;

private:
    CameraTimelineRenderContextHandle mPrevious;
};

void publishCameraTimeline(CameraTimelineSource source, CameraTimelineHandle timeline);
void clearCameraTimeline(CameraTimelineSource source, CameraTimelineHandle const& expected = {});

[[nodiscard]] std::optional<CameraTimelineSample> sampleCameraTimeline(
    CameraTimelineSource                       source,
    visuals::ReplaySampleTime const& time,
    std::string_view                           cameraId = {}
) noexcept;

[[nodiscard]] bool hasCameraTimeline(CameraTimelineSource source) noexcept;

[[nodiscard]] CameraTimelineRenderContextHandle
publishCameraTimelineRenderContext(CameraTimelineRenderContext context);
void clearCameraTimelineRenderContext(
    CameraTimelineSource                    source,
    CameraTimelineRenderContextHandle const& expected = {}
) noexcept;

[[nodiscard]] CameraTimelineRenderContextHandle currentCameraTimelineRenderContext() noexcept;

} // namespace playback::editor::keyframe
