#pragma once

#include "ExportKeyframeApplier.h"
#include "ExportTypes.h"
#include "OfflineRenderClockHooks.h"

#include <cstdint>
#include <optional>
#include <string>

namespace playback::exporting {

enum class OfflineRenderFrameExecutionResult : uint8_t { Waiting, Executed, Failed };

struct OfflineRenderFrameExecutorStatus {
    bool        open{};
    bool        renderSizeChanged{};
    bool        uiStable{};
    uint32_t    renderWidth{};
    uint32_t    renderHeight{};
    std::string message;
};

class OfflineRenderFrameExecutor {
public:
    OfflineRenderFrameExecutor() = default;
    ~OfflineRenderFrameExecutor();

    OfflineRenderFrameExecutor(OfflineRenderFrameExecutor const&)            = delete;
    OfflineRenderFrameExecutor& operator=(OfflineRenderFrameExecutor const&) = delete;

    [[nodiscard]] bool open(
        ExportSettings const&                        settings,
        state::editing::model::EditorStateExt const& project,
        std::optional<std::string>                   cameraFallback = std::nullopt
    );
    void close();

    [[nodiscard]] OfflineRenderFrameExecutionResult
    executeSample(ExportFramePlan const& frame, OfflineRenderClockToken clockToken);
    [[nodiscard]] OfflineRenderFrameExecutionResult executeWarmup(OfflineRenderClockToken clockToken);
    void                                            completeWarmup();
    void                                            completeSample(visuals::FrameTicket const& ticket);
    void                                            pollCapture();

    [[nodiscard]] OfflineRenderFrameExecutorStatus status() const;
    [[nodiscard]] bool                             isUiStable() const;

private:
    [[nodiscard]] bool configureRenderSize(ExportSettings const& settings);
    void               restoreRenderSize();
    [[nodiscard]] bool configureClientThrottling();
    void               restoreClientThrottling();
    [[nodiscard]] bool prepareNativeRender();
    void               fail(std::string message);

    ExportKeyframeApplier               mKeyframes;
    std::optional<visuals::FrameTicket> mPendingTicket;
    uint32_t                            mRestoreRenderWidth{};
    uint32_t                            mRestoreRenderHeight{};
    uint32_t                            mRestoreUiWidth{};
    uint32_t                            mRestoreUiHeight{};
    uint32_t                            mRenderWidth{};
    uint32_t                            mRenderHeight{};
    float                               mRestoreGuiScale{};
    float                               mRestoreViewportWidth{};
    float                               mRestoreViewportHeight{};
    float                               mRestoreViewportOffsetX{};
    float                               mRestoreViewportOffsetY{};
    float                               mRestoreViewportMinDepth{};
    float                               mRestoreViewportMaxDepth{};
    int                                 mRestoreThrottleThreshold{};
    float                               mRestoreThrottleScalar{};
    bool                                mOpen{};
    bool                                mRenderSizeChanged{};
    bool                                mUiStable{};
    bool                                mRestoreLowFrequencyUiRender{};
    bool                                mRestoreThrottleEnabled{};
    bool                                mClientThrottlingConfigured{};
    bool                                mSampleRenderInvoked{};
    bool                                mWarmupRenderInvoked{};
    std::string                         mMessage;
};

} // namespace playback::exporting
