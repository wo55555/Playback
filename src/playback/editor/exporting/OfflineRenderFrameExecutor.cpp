#include "OfflineRenderFrameExecutor.h"

#include "playback/Playback.h"
#include "playback/editor/renderer/ImGuiRenderer.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/gui/GuiData.h"
#include "mc/deps/renderer/ViewportInfo.h"
#include "mc/util/Timer.h"

#include <cmath>
#include <limits>
#include <utility>

namespace playback::editor::exporting {

namespace {

constexpr float ReplayTicksPerSecond = 20.0f;
thread_local bool gInvokingOfflineRender{};

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

bool OfflineRenderFrameExecutor::open(ExportSettings const& settings, editing::model::EditorStateExt const& project) {
    close();
    std::optional<float> aspectRatio;
    if (settings.resolutionX != 0 && settings.resolutionY != 0) {
        aspectRatio = static_cast<float>(settings.resolutionX) / static_cast<float>(settings.resolutionY);
    }
    mKeyframes.configure(project, aspectRatio);
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
    mOpen                = false;
    mRenderInvoked       = false;
    mWarmupRenderInvoked = false;
    mMessage.clear();
}

bool OfflineRenderFrameExecutor::configureClientThrottling() {
    auto client = ll::service::getClientInstance();
    if (!client) {
        fail("The Bedrock client is unavailable while configuring offline rendering");
        return false;
    }

    mRestoreThrottleEnabled   = client->isClientUpdateAndRenderThrottlingEnabled();
    mRestoreThrottleThreshold = client->getClientUpdateAndRenderThrottlingThreshold();
    mRestoreThrottleScalar    = client->getClientUpdateAndRenderThrottlingScalar();
    client->setClientUpdateAndRenderThrottling(false, mRestoreThrottleThreshold, mRestoreThrottleScalar);
    mClientThrottlingConfigured = true;
    return true;
}

void OfflineRenderFrameExecutor::restoreClientThrottling() {
    if (!mClientThrottlingConfigured) return;
    if (auto client = ll::service::getClientInstance()) {
        client->setClientUpdateAndRenderThrottling(
            mRestoreThrottleEnabled,
            mRestoreThrottleThreshold,
            mRestoreThrottleScalar
        );
    }
    mClientThrottlingConfigured = false;
    mRestoreThrottleEnabled     = false;
    mRestoreThrottleThreshold   = 0;
    mRestoreThrottleScalar      = 0.0f;
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
        mPendingTicket = frame.ticket;
        mRenderInvoked = false;
    }

    pollCapture();

    if (!mRenderInvoked) {
        if (!invokeBedrockRender()) return OfflineRenderFrameExecutionResult::Failed;
        mRenderInvoked = true;
        if (!wasOfflineRenderClockSampleApplied(clockToken)) {
            fail("The explicit Bedrock render returned without applying its fractional clock sample");
            return OfflineRenderFrameExecutionResult::Failed;
        }
    }
    return OfflineRenderFrameExecutionResult::Executed;
}

OfflineRenderFrameExecutionResult
OfflineRenderFrameExecutor::executeWarmup(ExportFramePlan const& frame, OfflineRenderClockToken clockToken) {
    if (!mOpen || !clockToken || mPendingTicket) {
        fail("The offline frame executor cannot render a warm-up frame in its current state");
        return OfflineRenderFrameExecutionResult::Failed;
    }
    if (!mWarmupRenderInvoked) {
        (void)frame;
        if (!invokeBedrockRender()) return OfflineRenderFrameExecutionResult::Failed;
        mWarmupRenderInvoked = true;
        if (!wasOfflineRenderClockSampleApplied(clockToken)) {
            fail("The warm-up Bedrock render returned without applying its fractional clock sample");
            return OfflineRenderFrameExecutionResult::Failed;
        }
    }
    return OfflineRenderFrameExecutionResult::Executed;
}

void OfflineRenderFrameExecutor::completeWarmup() { mWarmupRenderInvoked = false; }

void OfflineRenderFrameExecutor::completeSample(functions::render::FrameTicket const& ticket) {
    if (!mPendingTicket || !ticketsEqual(*mPendingTicket, ticket)) return;
    mPendingTicket.reset();
    mRenderInvoked = false;
}

void OfflineRenderFrameExecutor::pollCapture() { renderer::gImGuiRenderer.pollFrameCapture(); }

bool OfflineRenderFrameExecutor::invokeBedrockRender() {
    if (gInvokingOfflineRender) {
        fail("The offline Bedrock render call attempted to re-enter itself");
        return false;
    }
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

    Timer timer(ReplayTicksPerSecond, [] { return Timer::getMillisecondsSinceLaunch(); });

    auto& game = static_cast<MinecraftGame&>(client->getMinecraftGame_DEPRECATED());
    Bedrock::NotNullNonOwnerPtr<IClientInstance> clientPointer{static_cast<IClientInstance&>(*client)};
    struct RenderInvocationGuard {
        bool& active;
        ~RenderInvocationGuard() { active = false; }
    } guard{gInvokingOfflineRender};
    gInvokingOfflineRender = true;
    game.updateGraphics(clientPointer, timer);
    return true;
}

OfflineRenderFrameExecutorStatus OfflineRenderFrameExecutor::status() const {
    return {mOpen, mRenderSizeChanged, mRenderWidth, mRenderHeight, mMessage};
}

void OfflineRenderFrameExecutor::fail(std::string message) { mMessage = std::move(message); }

} // namespace playback::editor::exporting
