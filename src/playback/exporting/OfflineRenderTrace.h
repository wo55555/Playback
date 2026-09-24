#pragma once

#include <cstdint>
#include <filesystem>

#include "RenderDiagnostics.h"

namespace playback::visuals {
struct CapturedFrame;
}

namespace playback::exporting {

enum class OfflineRenderTraceEvent : uint8_t {
    SessionBegin,
    SessionEnd,
    ClockPublished,
    ClockPermitted,
    ClockCleared,
    GraphicsEnter,
    GraphicsExit,
    GraphicsSkipped,
    GameRenderEnter,
    GameRenderExit,
    ApiFrameEnter,
    ApiFrameExit,
    RenderFrameEnter,
    RenderFrameExit,
    SubmitD3D12Enter,
    SubmitD3D12Exit,
    SubmitD3D11Enter,
    SubmitD3D11Exit,
    PresentEnter,
    PresentExit,
    Present1Enter,
    Present1Exit,
    CaptureArm,
    CaptureGate,
    CaptureSource,
    CaptureRecorded,
    CaptureFence,
    ReadbackReady,
    CaptureCollected,
    WarmupComplete,
    BoundaryFault,
    UpscalingEnter,
    UpscalingExit,
    UpscalingState,
    HookInventory,
    RenderState,
    ClientRenderState,
    ResourceState,
    PreviewSource,
    PreviewCopy,
    PreviewDisplay,
    UpscalingConfig,
    CameraState,
    CameraLens,
    CameraSample,
    SubmissionGate,
    ExtractFrameEnter,
    ExtractFrameExit,
    ExtractedFrame,
    ExtractedView,
    GameEndFrameEnter,
    GameEndFrameExit,
    ReadbackCopy,
    CapturePixelLayout,
    CapturePixels,
    CapturePixelsInvalid,
    CpuSubmissionPermit,
    CaptureLuma,
    CaptureLumaDistribution,
    CaptureLumaTile,
    WorldEnvironment,
    WorldClock,
    WeatherRain,
    WeatherLightning,
    WeatherSample,
    ClockSample,
    GraphicsTimer,
    GraphicsTimerAbsolute,
    CameraForward,
    CameraUp,
    CameraViewForward,
    CameraViewUp,
    DriverTickEnter,
    DriverTickExit,
    DriverStage,
    DriverCollectEnter,
    DriverCollectExit,
    DriverAdvanceEnter,
    DriverAdvanceExit,
    BoundaryState,
    WriterSubmitEnter,
    WriterSubmitExit,
    WriterQueue,
    WriterWorkEnter,
    WriterWorkExit,
    WriterNormalizeEnter,
    WriterNormalizeExit,
    WriterPipeEnter,
    WriterPipeExit,
    WriterLaunchEnter,
    WriterLaunchExit,
    DriverWait,
    GraphicsDecision,
    RendererClock,
    ReadbackFenceObserved,
    ReadbackMapEnter,
    ReadbackMapExit,
    ReadbackCpuCopyEnter,
    ReadbackCpuCopyExit,
    WriterPipeTiming,
    WriterPipeChunks,
    ExtractedViewInputs,
    RendererClockIdentity,
    FrameBuilderTiming,
    GraphicsHookEnter,
    NativeSubmitEnter,
    NativeSubmitExit,
    NativeSubmitClassify,
    NativeSubmitViews,
    CaptureSubmitObserved,
    CaptureSourceLink,
    CaptureQueueExecute,
    CaptureQueueSignal,
    CaptureFenceLink,
    CaptureReadyBeforeComplete,
    CaptureCollectedLink,
    SceneMarkerSet,
    SceneMarkerCapture,
    RenderKeepAlive,
    WriterStageStarve,
    WriterStageDepth,
    Count,
};

enum class OfflineRenderWaitReason : uint8_t {
    None,
    WriterBackpressure,
    CaptureCapacity,
    ReplayPreparation,
    DimensionTransition,
    UiStable,
    NativeTick,
    WarmupCpu,
    WarmupBudget,
    WarmupUi,
    CaptureArm,
    CpuSample,
    CapturePending,
    CollectPending,
    Draining,
    Failed,
    Convergence,
    Unknown,
    Count,
};

[[nodiscard]] bool     beginOfflineRenderTrace(std::filesystem::path const& path) noexcept;
[[nodiscard]] bool     finishOfflineRenderTrace() noexcept;
[[nodiscard]] bool     isOfflineRenderTraceActive() noexcept;
[[nodiscard]] uint64_t offlineRenderTraceEpoch() noexcept;

struct OfflineSubmitObservation {
    uint64_t active{};
    uint64_t lastReturned{};
};

[[nodiscard]] OfflineSubmitObservation offlineSubmitObservation(uint64_t epoch) noexcept;

class OfflineRenderSubmitScope {
public:
    OfflineRenderSubmitScope(
        RenderDiagnosticProfile profile,
        void const*             frame,
        void const*             renderer,
        uint64_t                generation,
        bool                    cpuReady,
        bool                    carriesScene,
        uint32_t                backend,
        uint64_t                expectedEpoch = UINT64_MAX
    ) noexcept;
    ~OfflineRenderSubmitScope() noexcept;
    void                   returned(bool accepted) noexcept;
    [[nodiscard]] uint64_t serial() const noexcept { return mSerial; }
    [[nodiscard]] uint64_t epoch() const noexcept { return mEpoch; }
    OfflineRenderSubmitScope(OfflineRenderSubmitScope const&)            = delete;
    OfflineRenderSubmitScope& operator=(OfflineRenderSubmitScope const&) = delete;

private:
    uint64_t    mEpoch{}, mSerial{}, mPrevious{}, mGeneration{}, mFlags{};
    void const* mFrame{};
    void const* mRenderer{};
    bool        mReturned{};
    bool        mAccepted{};
};

struct OfflineNativeSubmissionSummary {
    uint64_t items{UINT64_MAX};
    uint64_t inspected{};
    uint64_t lastDeclaration{UINT64_MAX};
    uint64_t viewRemap{};
    bool     hasViews{};
};

void recordNativeSubmissionSummary(
    RenderDiagnosticProfile               profile,
    OfflineRenderSubmitScope const&       scope,
    void const*                           frame,
    OfflineNativeSubmissionSummary const& summary,
    bool                                  returned = false
) noexcept;

void recordCaptureLineage(
    RenderDiagnosticProfile profile,
    uint64_t                epoch,
    OfflineRenderTraceEvent event,
    void const*             object  = nullptr,
    void const*             related = nullptr,
    uint64_t                a       = 0,
    uint64_t                b       = 0,
    uint64_t                c       = 0,
    uint64_t                d       = 0
) noexcept;

void recordSceneCorrespondence(
    RenderDiagnosticProfile profile,
    uint64_t                epoch,
    OfflineRenderTraceEvent event,
    void const*             object  = nullptr,
    void const*             related = nullptr,
    uint64_t                a       = 0,
    uint64_t                b       = 0,
    uint64_t                c       = 0,
    uint64_t                d       = 0
) noexcept;

void recordOfflineRenderTraceForEpoch(
    uint64_t                epoch,
    OfflineRenderTraceEvent event,
    void const*             object  = nullptr,
    void const*             related = nullptr,
    uint64_t                a       = 0,
    uint64_t                b       = 0,
    uint64_t                c       = 0,
    uint64_t                d       = 0
) noexcept;

void recordOfflineRenderPixels(visuals::CapturedFrame const& frame) noexcept;

void setOfflineRenderTraceSample(uint64_t token, uint64_t frameIndex) noexcept;
void recordOfflineRenderTrace(
    OfflineRenderTraceEvent event,
    void const*             object  = nullptr,
    void const*             related = nullptr,
    uint64_t                a       = 0,
    uint64_t                b       = 0,
    uint64_t                c       = 0,
    uint64_t                d       = 0
) noexcept;

// Establish and destroy on the same Graphics thread; the sample never propagates to other threads.
class ScopedOfflineRenderTraceSample {
public:
    ScopedOfflineRenderTraceSample(uint64_t token, uint64_t frameIndex, uint64_t renderSerial) noexcept;
    ~ScopedOfflineRenderTraceSample() noexcept;

    ScopedOfflineRenderTraceSample(ScopedOfflineRenderTraceSample const&) noexcept            = delete;
    ScopedOfflineRenderTraceSample& operator=(ScopedOfflineRenderTraceSample const&) noexcept = delete;
    ScopedOfflineRenderTraceSample(ScopedOfflineRenderTraceSample&&) noexcept                 = delete;
    ScopedOfflineRenderTraceSample& operator=(ScopedOfflineRenderTraceSample&&) noexcept      = delete;

private:
    uint64_t mEpoch{};
    uint64_t mPreviousToken{};
    uint64_t mPreviousFrame{};
    uint64_t mPreviousRenderSerial{};
};

// Construct and destroy on the same thread in nesting order.
class OfflineRenderTraceScope {
public:
    OfflineRenderTraceScope(
        OfflineRenderTraceEvent enter,
        OfflineRenderTraceEvent exit,
        void const*             object        = nullptr,
        void const*             related       = nullptr,
        uint64_t                a             = 0,
        uint64_t                b             = 0,
        uint64_t                c             = 0,
        uint64_t                d             = 0,
        uint64_t                expectedEpoch = UINT64_MAX
    ) noexcept;
    ~OfflineRenderTraceScope() noexcept;

    void result(uint64_t value) noexcept;

    OfflineRenderTraceScope(OfflineRenderTraceScope const&) noexcept            = delete;
    OfflineRenderTraceScope& operator=(OfflineRenderTraceScope const&) noexcept = delete;
    OfflineRenderTraceScope(OfflineRenderTraceScope&&) noexcept                 = delete;
    OfflineRenderTraceScope& operator=(OfflineRenderTraceScope&&) noexcept      = delete;

private:
    uint64_t                mEpoch{};
    uint64_t                mPreviousParent{};
    bool                    mStored{};
    uint64_t                mResult{};
    uint64_t                mB{};
    uint64_t                mC{};
    uint64_t                mD{};
    void const*             mObject{};
    void const*             mRelated{};
    OfflineRenderTraceEvent mExit{OfflineRenderTraceEvent::Count};
};

} // namespace playback::exporting
