#include "OfflineRenderFrameExecutor.h"

#include "playback/Playback.h"
#include "playback/editor/renderer/ImGuiRenderer.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/deps/renderer/ViewportInfo.h"

#include <cmath>
#include <limits>
#include <utility>

namespace playback::editor::exporting {

namespace {

bool ticketsEqual(functions::render::FrameTicket const& left, functions::render::FrameTicket const& right) {
    return left.frameIndex == right.frameIndex && left.ptsNumerator == right.ptsNumerator
        && left.ptsDenominator == right.ptsDenominator;
}

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

std::optional<std::pair<uint32_t, uint32_t>> currentRenderSize(ClientInstance& client) {
    auto const& viewport = *client.getViewportInfo().size;
    if (std::isfinite(viewport.x) && std::isfinite(viewport.y) && viewport.x >= 1.0f && viewport.y >= 1.0f
        && viewport.x <= std::numeric_limits<uint32_t>::max() && viewport.y <= std::numeric_limits<uint32_t>::max()) {
        return std::pair{
            static_cast<uint32_t>(std::lround(viewport.x)),
            static_cast<uint32_t>(std::lround(viewport.y)),
        };
    }

    auto const  gui  = client.getGuiData();
    auto const& size = *gui->mScreenSizeData->totalScreenSize;
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 1.0f || size.y < 1.0f
        || size.x > std::numeric_limits<uint32_t>::max() || size.y > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return std::pair{
        static_cast<uint32_t>(std::lround(size.x)),
        static_cast<uint32_t>(std::lround(size.y)),
    };
}

std::optional<std::pair<uint32_t, uint32_t>> currentUiSize(ClientInstance& client) {
    auto const  gui  = client.getGuiData();
    auto const& size = *gui->mScreenSizeData->totalScreenSize;
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 1.0f || size.y < 1.0f
        || size.x > std::numeric_limits<uint32_t>::max() || size.y > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return std::pair{
        static_cast<uint32_t>(std::lround(size.x)),
        static_cast<uint32_t>(std::lround(size.y)),
    };
}

} // namespace

OfflineRenderFrameExecutor::~OfflineRenderFrameExecutor() { close(); }

bool OfflineRenderFrameExecutor::open(
    ExportSettings const& settings,
    editing::model::EditorStateExt const& project,
    std::optional<std::string> cameraFallback
) {
    close();
    std::optional<float> aspectRatio;
    if (settings.resolutionX != 0 && settings.resolutionY != 0) {
        aspectRatio = static_cast<float>(settings.resolutionX) / static_cast<float>(settings.resolutionY);
    }
    mKeyframes.configure(project, aspectRatio, std::move(cameraFallback));
    if (!configureClientThrottling() || !configureRenderSize(settings)) {
        restoreClientThrottling();
        mKeyframes.reset();
        return false;
    }
    mOpen = true;
    return true;
}

void OfflineRenderFrameExecutor::close() {
    restoreRenderSize();
    restoreClientThrottling();
    mKeyframes.reset();
    mPendingTicket.reset();
    mRenderWidth         = 0;
    mRenderHeight        = 0;
    mUiStable            = false;
    mOpen                = false;
    mSampleRenderInvoked = false;
    mWarmupRenderInvoked = false;
    mMessage.clear();
}

bool OfflineRenderFrameExecutor::configureClientThrottling() {
    auto client = ll::service::getClientInstance();
    if (!client) {
        fail("The Bedrock client is unavailable while configuring offline rendering");
        return false;
    }

    mRestoreThrottleEnabled           = client->isClientUpdateAndRenderThrottlingEnabled();
    mRestoreThrottleThreshold         = client->getClientUpdateAndRenderThrottlingThreshold();
    mRestoreThrottleScalar            = client->getClientUpdateAndRenderThrottlingScalar();
    auto& renderer                    = client->getGameRenderer();
    mRestoreLowFrequencyUiRender      = renderer.mUseLowFrequencyUIRender;
    renderer.mUseLowFrequencyUIRender = false;
    client->setClientUpdateAndRenderThrottling(false, mRestoreThrottleThreshold, mRestoreThrottleScalar);
    mClientThrottlingConfigured = true;
    getLogger().info(
        "Offline client render scheduling configured: throttling=false, lowFrequencyUi=false (previous={})",
        mRestoreLowFrequencyUiRender
    );
    return true;
}

void OfflineRenderFrameExecutor::restoreClientThrottling() {
    if (!mClientThrottlingConfigured) return;
    if (auto client = ll::service::getClientInstance()) {
        client->getGameRenderer().mUseLowFrequencyUIRender = mRestoreLowFrequencyUiRender;
        client->setClientUpdateAndRenderThrottling(
            mRestoreThrottleEnabled,
            mRestoreThrottleThreshold,
            mRestoreThrottleScalar
        );
    }
    mClientThrottlingConfigured  = false;
    mRestoreLowFrequencyUiRender = false;
    mRestoreThrottleEnabled      = false;
    mRestoreThrottleThreshold    = 0;
    mRestoreThrottleScalar       = 0.0f;
}

bool OfflineRenderFrameExecutor::configureRenderSize(ExportSettings const& settings) {
    if (settings.resolutionX == 0 && settings.resolutionY == 0) return true;

    auto client = ll::service::getClientInstance();
    if (!client) {
        fail("The Bedrock client is unavailable while configuring the export render size");
        return false;
    }
    auto const current = currentRenderSize(*client);
    if (!current) {
        fail("The current Bedrock render size is unavailable");
        return false;
    }
    auto const uiSize = currentUiSize(*client).value_or(*current);

    auto const& viewport         = client->getViewportInfo();
    auto const& viewportSize     = *viewport.size;
    auto const& viewportOffset   = *viewport.offset;
    float const viewportMinDepth = viewport.minDepth;
    float const viewportMaxDepth = viewport.maxDepth;
    float const guiScale         = client->getGuiData()->getGuiScale();
    float const restoreViewportWidth =
        std::isfinite(viewportSize.x) && viewportSize.x >= 1.0f ? viewportSize.x : static_cast<float>(current->first);
    float const restoreViewportHeight =
        std::isfinite(viewportSize.y) && viewportSize.y >= 1.0f ? viewportSize.y : static_cast<float>(current->second);
    float const restoreViewportOffsetX  = std::isfinite(viewportOffset.x) ? viewportOffset.x : 0.0f;
    float const restoreViewportOffsetY  = std::isfinite(viewportOffset.y) ? viewportOffset.y : 0.0f;
    float const restoreViewportMinDepth = std::isfinite(viewportMinDepth) ? viewportMinDepth : 0.0f;
    float const restoreViewportMaxDepth = std::isfinite(viewportMaxDepth) ? viewportMaxDepth : 1.0f;
    float const restoreGuiScale         = std::isfinite(guiScale) && guiScale > 0.0f ? guiScale : 0.0f;

    uint64_t const renderWidth  = static_cast<uint64_t>(settings.resolutionX) * settings.ssaa;
    uint64_t const renderHeight = static_cast<uint64_t>(settings.resolutionY) * settings.ssaa;
    if (renderWidth == 0 || renderHeight == 0 || renderWidth > std::numeric_limits<int>::max()
        || renderHeight > std::numeric_limits<int>::max()) {
        fail("The supersampled Bedrock render size is invalid");
        return false;
    }

    mRestoreRenderWidth      = current->first;
    mRestoreRenderHeight     = current->second;
    mRestoreUiWidth          = uiSize.first;
    mRestoreUiHeight         = uiSize.second;
    mRestoreGuiScale         = restoreGuiScale;
    mRestoreViewportWidth    = restoreViewportWidth;
    mRestoreViewportHeight   = restoreViewportHeight;
    mRestoreViewportOffsetX  = restoreViewportOffsetX;
    mRestoreViewportOffsetY  = restoreViewportOffsetY;
    mRestoreViewportMinDepth = restoreViewportMinDepth;
    mRestoreViewportMaxDepth = restoreViewportMaxDepth;
    mRenderWidth             = static_cast<uint32_t>(renderWidth);
    mRenderHeight            = static_cast<uint32_t>(renderHeight);
    auto& game               = client->getMinecraftGame_DEPRECATED();
    game.setRenderingSize(static_cast<int>(mRenderWidth), static_cast<int>(mRenderHeight));
    game.setUISizeAndScale(static_cast<int>(mRenderWidth), static_cast<int>(mRenderHeight), 0.0f);

    // setRenderingSize changes the swap-chain target, but the client viewport
    // can still describe the old window-sized region. Make the active render
    // area explicit so SSAA fills the whole captured surface.
    auto exportViewport      = viewport;
    exportViewport.size->x   = static_cast<float>(mRenderWidth);
    exportViewport.size->y   = static_cast<float>(mRenderHeight);
    exportViewport.offset->x = 0.0f;
    exportViewport.offset->y = 0.0f;
    client->setViewportInfo(exportViewport);

    mRenderSizeChanged = true;
    getLogger().info(
        "Offline render surface configured: display={}x{}, viewport={}x{} at ({}, {}), render={}x{}, guiScale={}",
        mRestoreUiWidth,
        mRestoreUiHeight,
        mRestoreViewportWidth,
        mRestoreViewportHeight,
        mRestoreViewportOffsetX,
        mRestoreViewportOffsetY,
        mRenderWidth,
        mRenderHeight,
        mRestoreGuiScale
    );
    return true;
}

void OfflineRenderFrameExecutor::restoreRenderSize() {
    if (!mRenderSizeChanged) return;
    auto client = ll::service::getClientInstance();
    if (client && mRestoreRenderWidth != 0 && mRestoreRenderHeight != 0 && mRestoreUiWidth != 0
        && mRestoreUiHeight != 0) {
        auto& game = client->getMinecraftGame_DEPRECATED();
        game.setRenderingSize(static_cast<int>(mRestoreRenderWidth), static_cast<int>(mRestoreRenderHeight));
        game.setUISizeAndScale(static_cast<int>(mRestoreUiWidth), static_cast<int>(mRestoreUiHeight), mRestoreGuiScale);

        auto viewport      = client->getViewportInfo();
        viewport.size->x   = mRestoreViewportWidth;
        viewport.size->y   = mRestoreViewportHeight;
        viewport.offset->x = mRestoreViewportOffsetX;
        viewport.offset->y = mRestoreViewportOffsetY;
        viewport.minDepth  = mRestoreViewportMinDepth;
        viewport.maxDepth  = mRestoreViewportMaxDepth;
        client->setViewportInfo(viewport);
    }
    mRenderSizeChanged       = false;
    mRestoreRenderWidth      = 0;
    mRestoreRenderHeight     = 0;
    mRestoreUiWidth          = 0;
    mRestoreUiHeight         = 0;
    mRestoreGuiScale         = 0.0f;
    mRestoreViewportWidth    = 0.0f;
    mRestoreViewportHeight   = 0.0f;
    mRestoreViewportOffsetX  = 0.0f;
    mRestoreViewportOffsetY  = 0.0f;
    mRestoreViewportMinDepth = 0.0f;
    mRestoreViewportMaxDepth = 0.0f;
}

OfflineRenderFrameExecutionResult
OfflineRenderFrameExecutor::executeSample(ExportFramePlan const& frame, OfflineRenderClockToken clockToken) {
    if (!mOpen || !clockToken) {
        fail("The offline frame executor is not ready");
        return OfflineRenderFrameExecutionResult::Failed;
    }
    if (mPendingTicket && !ticketsEqual(*mPendingTicket, frame.ticket)) {
        fail("The offline frame executor received a second sample before the first completed");
        return OfflineRenderFrameExecutionResult::Failed;
    }
    if (!mPendingTicket) {
        mPendingTicket       = frame.ticket;
        mSampleRenderInvoked = false;
    }

    pollCapture();
    if (!mSampleRenderInvoked) {
        if (!prepareNativeRender()) return OfflineRenderFrameExecutionResult::Failed;
        mSampleRenderInvoked = true;
    }
    bool const applied   = wasOfflineRenderClockSampleApplied(clockToken);
    bool const completed = wasOfflineRenderClockSampleCompleted(clockToken);
    if (!applied || !completed) return OfflineRenderFrameExecutionResult::Waiting;
    return OfflineRenderFrameExecutionResult::Executed;
}

OfflineRenderFrameExecutionResult OfflineRenderFrameExecutor::executeWarmup(OfflineRenderClockToken clockToken) {
    if (!mOpen || !clockToken || mPendingTicket) {
        fail("The offline frame executor cannot render a warm-up frame in its current state");
        return OfflineRenderFrameExecutionResult::Failed;
    }
    if (!mWarmupRenderInvoked) {
        if (!prepareNativeRender()) return OfflineRenderFrameExecutionResult::Failed;
        mWarmupRenderInvoked = true;
    }
    bool const applied   = wasOfflineRenderClockSampleApplied(clockToken);
    bool const completed = wasOfflineRenderClockSampleCompleted(clockToken);
    if (!applied || !completed) return OfflineRenderFrameExecutionResult::Waiting;
    mUiStable = isUiStable();
    return OfflineRenderFrameExecutionResult::Executed;
}

void OfflineRenderFrameExecutor::completeWarmup() { mWarmupRenderInvoked = false; }

void OfflineRenderFrameExecutor::completeSample(functions::render::FrameTicket const& ticket) {
    if (!mPendingTicket || !ticketsEqual(*mPendingTicket, ticket)) return;
    mPendingTicket.reset();
    mSampleRenderInvoked = false;
}

void OfflineRenderFrameExecutor::pollCapture() { renderer::gImGuiRenderer.pollFrameCapture(); }

bool OfflineRenderFrameExecutor::prepareNativeRender() {
    auto client = ll::service::getClientInstance();
    if (!client) {
        fail("The Bedrock client is unavailable for offline rendering");
        return false;
    }

    if (mRenderWidth != 0 && mRenderHeight != 0) {
        auto viewport      = client->getViewportInfo();
        viewport.size->x   = static_cast<float>(mRenderWidth);
        viewport.size->y   = static_cast<float>(mRenderHeight);
        viewport.offset->x = 0.0f;
        viewport.offset->y = 0.0f;
        client->setViewportInfo(viewport);
    }

    // ReplayExportDriver runs immediately before ClientInstance::update. The
    // published sample is consumed by that one native graphics pass, which
    // preserves Bedrock's full scene preparation and render ordering.
    return true;
}

OfflineRenderFrameExecutorStatus OfflineRenderFrameExecutor::status() const {
    return {mOpen, mRenderSizeChanged, mUiStable, mRenderWidth, mRenderHeight, mMessage};
}

bool OfflineRenderFrameExecutor::isUiStable() const {
    auto  client = ll::service::getClientInstance();
    auto& replay = functions::ReplaySession::getInstance();
    return client && replay.isActive() && replay.hasJoinedReplayWorld() && !replay.isDimensionTransitionPending()
        && client->isInWorldAndNotShowingAnyMenuScreens() && !client->isShowingLoadingScreen()
        && !client->isShowingProgressScreen();
}

void OfflineRenderFrameExecutor::fail(std::string message) { mMessage = std::move(message); }

} // namespace playback::editor::exporting
