#pragma once

#include "ExportKeyframeApplier.h"
#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"

#include <cstdint>
#include <optional>
#include <string>

namespace playback::editor::exporting {

enum class OfflineRenderFrameExecutionResult : uint8_t { Waiting, Executed, Failed };

struct OfflineRenderFrameExecutorStatus {
    bool        open{};
    bool        renderSizeChanged{};
    uint32_t    renderWidth{};
    uint32_t    renderHeight{};
    std::string message;
};

// Owns one Bedrock render request. Replay preparation and FrameTap completion
// stay in the surrounding offline-render boundary.
class OfflineRenderFrameExecutor {
public:
    OfflineRenderFrameExecutor() = default;
    ~OfflineRenderFrameExecutor();

    OfflineRenderFrameExecutor(OfflineRenderFrameExecutor const&)            = delete;
    OfflineRenderFrameExecutor& operator=(OfflineRenderFrameExecutor const&) = delete;

    [[nodiscard]] bool open(ExportSettings const& settings, editing::model::EditorStateExt const& project);
    void               close();

    [[nodiscard]] OfflineRenderFrameExecutionResult
    executeSample(ExportFramePlan const& frame, OfflineRenderClockToken clockToken);
    [[nodiscard]] OfflineRenderFrameExecutionResult
         executeWarmup(ExportFramePlan const& frame, OfflineRenderClockToken clockToken);
    void completeWarmup();
    void completeSample(functions::render::FrameTicket const& ticket);
    void pollCapture();

    [[nodiscard]] OfflineRenderFrameExecutorStatus status() const;

private:
    [[nodiscard]] bool configureRenderSize(ExportSettings const& settings);
    void               restoreRenderSize();
    [[nodiscard]] bool configureClientThrottling();
    void               restoreClientThrottling();
    [[nodiscard]] bool invokeBedrockRender();
    void               fail(std::string message);

    ExportKeyframeApplier                         mKeyframes;
    std::optional<functions::render::FrameTicket> mPendingTicket;
    uint32_t                                      mRestoreRenderWidth{};
    uint32_t                                      mRestoreRenderHeight{};
    uint32_t                                      mRestoreUiWidth{};
    uint32_t                                      mRestoreUiHeight{};
    uint32_t                                      mRenderWidth{};
    uint32_t                                      mRenderHeight{};
    float                                         mRestoreGuiScale{};
    float                                         mRestoreViewportWidth{};
    float                                         mRestoreViewportHeight{};
    float                                         mRestoreViewportOffsetX{};
    float                                         mRestoreViewportOffsetY{};
    float                                         mRestoreViewportMinDepth{};
    float                                         mRestoreViewportMaxDepth{};
    int                                           mRestoreThrottleThreshold{};
    float                                         mRestoreThrottleScalar{};
    bool                                          mOpen{};
    bool                                          mRenderSizeChanged{};
    bool                                          mRestoreThrottleEnabled{};
    bool                                          mClientThrottlingConfigured{};
    bool                                          mRenderInvoked{};
    bool                                          mWarmupRenderInvoked{};
    std::string                                   mMessage;
};

} // namespace playback::editor::exporting
