#pragma once

#include <cstdint>

namespace playback::exporting {

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

} // namespace playback::exporting
