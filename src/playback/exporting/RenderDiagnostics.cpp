#include "RenderDiagnostics.h"

#include "ExportActivity.h"
#include "OfflineRenderTrace.h"
#include "playback/Playback.h"
#include "playback/replay/ReplaySession.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/common/Globals.h"
#include "mc/deps/minecraft_renderer/framebuilder/FrameBuilder.h"
#include "mc/deps/renderer/ViewportInfo.h"
#include "mc/network/packet/PackInfoData.h"
#include "mc/network/packet/ResourcePackStackPacket.h"
#include "mc/network/packet/ResourcePacksInfoPacket.h"
#include "mc/platform/UUID.h"
#include "mc/resources/PackInstance.h"
#include "mc/resources/PackManifest.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/LevelSettings.h"
#include "mc/world/level/Weather.h"
#include "mc/world/level/dimension/Dimension.h"


#include <Windows.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace playback::exporting {
namespace {

std::string_view stageName(RenderDiagnosticStage stage) {
    switch (stage) {
    case RenderDiagnosticStage::ReplayStart:
        return "ReplayStart";
    case RenderDiagnosticStage::ReplayPacksPrepared:
        return "ReplayPacksPrepared";
    case RenderDiagnosticStage::ReplayWorldJoined:
        return "ReplayWorldJoined";
    case RenderDiagnosticStage::ReplayWorldReady:
        return "ReplayWorldReady";
    case RenderDiagnosticStage::EditorShown:
        return "EditorShown";
    case RenderDiagnosticStage::EditorHidden:
        return "EditorHidden";
    case RenderDiagnosticStage::ClientPulse:
        return "ClientPulse";
    case RenderDiagnosticStage::ExportBeforeOpen:
        return "ExportBeforeOpen";
    case RenderDiagnosticStage::ExportAfterOpen:
        return "ExportAfterOpen";
    case RenderDiagnosticStage::ResizeBefore:
        return "ResizeBefore";
    case RenderDiagnosticStage::ResizeAfter:
        return "ResizeAfter";
    case RenderDiagnosticStage::RestoreBefore:
        return "RestoreBefore";
    case RenderDiagnosticStage::RestoreAfter:
        return "RestoreAfter";
    case RenderDiagnosticStage::GraphicsBefore:
        return "GraphicsBefore";
    case RenderDiagnosticStage::GraphicsAfter:
        return "GraphicsAfter";
    case RenderDiagnosticStage::ExportClosed:
        return "ExportClosed";
    }
    return "Unknown";
}

struct RendererSnapshot {
    void const* builder{};
    uint64_t    flags{};
    uint64_t    width{};
    uint64_t    height{};
    float       upscaleFactor{};
};

std::atomic<uint64_t> gAdmissionSession{};
std::atomic<uint64_t> gAdmissionSequence{};
std::atomic<uint64_t> gClientSnapshotRequest{};

auto& diagnosticLogger() { return Playback::getInstance().getSelf().getLogger(); }

void reportDiagnosticFailure() noexcept {
    static std::atomic<bool> reported{};
    if (reported.exchange(true)) return;
    try {
        diagnosticLogger().warn("[ReplayAdmission] diagnostic failed; further diagnostic failures suppressed");
    } catch (...) {}
}

std::string experimentBits(std::vector<bool> const& values) {
    std::string result;
    for (size_t i = 0; i < values.size() && i < 256; ++i) result += values[i] ? '1' : '0';
    if (values.size() > 256) result += "...(truncated)";
    return result;
}

} // namespace

bool renderDiagnosticsEnabled() noexcept { return Playback::getInstance().getConfig().renderDiagnostics; }

void recordWorldEnvironment(
    WorldDiagnosticStage stage,
    Level*               level,
    Dimension*           dimension,
    float                partialTick,
    uint64_t             tickToken
) noexcept try {
    if (!renderDiagnosticsEnabled() || !isOfflineRenderTraceActive()) return;
    auto const* weather = dimension ? dimension->mWeather.get() : nullptr;
    auto const  phase   = static_cast<uint64_t>(stage);
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::WorldEnvironment,
        level,
        dimension,
        phase,
        (level ? 1ull : 0ull) | (dimension ? 2ull : 0ull) | (weather ? 4ull : 0ull),
        std::bit_cast<uint32_t>(partialTick),
        tickToken
    );
    if (level) {
        recordOfflineRenderTrace(
            OfflineRenderTraceEvent::WorldClock,
            level,
            dimension,
            phase,
            static_cast<uint64_t>(static_cast<int64_t>(level->getTime())),
            static_cast<uint64_t>(static_cast<int64_t>(replay::ReplaySession::getInstance().getAppliedReplayTick())),
            tickToken
        );
    }
    if (!weather) return;
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::WeatherRain,
        weather,
        dimension,
        phase,
        std::bit_cast<uint32_t>(static_cast<float>(weather->mOldRainLevel)),
        std::bit_cast<uint32_t>(static_cast<float>(weather->mRainLevel)),
        std::bit_cast<uint32_t>(static_cast<float>(weather->mTargetRainLevel))
    );
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::WeatherLightning,
        weather,
        dimension,
        phase,
        std::bit_cast<uint32_t>(static_cast<float>(weather->mOldLightningLevel)),
        std::bit_cast<uint32_t>(static_cast<float>(weather->mLightningLevel)),
        std::bit_cast<uint32_t>(static_cast<float>(weather->mTargetLightningLevel))
    );
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::WeatherSample,
        weather,
        dimension,
        phase,
        std::bit_cast<uint32_t>(weather->getRainLevel(partialTick)),
        std::bit_cast<uint32_t>(weather->getLightningLevel(partialTick)),
        static_cast<uint64_t>(static_cast<int64_t>(weather->getSkyFlashTime()))
    );
} catch (...) {
    reportDiagnosticFailure();
}

void recordRenderDiagnostics(RenderDiagnosticStage stage, ClientInstance* client) noexcept try {
    if (!renderDiagnosticsEnabled()) return;
    if (stage == RenderDiagnosticStage::ReplayStart) {
        ++gAdmissionSession;
        recordReplayAdmissionBoundary("Replay.begin");
    }
    auto const session = gAdmissionSession.load();
    bool const graphics =
        stage == RenderDiagnosticStage::GraphicsBefore || stage == RenderDiagnosticStage::GraphicsAfter;
    bool const periodic = graphics || stage == RenderDiagnosticStage::ClientPulse;

    auto const                                         now = std::chrono::steady_clock::now();
    thread_local std::chrono::steady_clock::time_point lastClientPulse;
    thread_local uint64_t                              lastClientRequest{};
    auto const                                         request        = gClientSnapshotRequest.load();
    bool const                                         requestChanged = request != lastClientRequest;
    auto const                                         afterEvent     = gAdmissionSequence.load();
    if (stage == RenderDiagnosticStage::ClientPulse) {
        auto const interval = replay::ReplaySession::getInstance().isReplayWorldReady()
                                ? std::chrono::milliseconds(2000)
                                : std::chrono::milliseconds(250);
        if (!requestChanged && now - lastClientPulse < interval) return;
        lastClientPulse   = now;
        lastClientRequest = request;
    }

    RendererSnapshot snapshot;
    auto*            builder = renderDragonFrameBuilder();
    snapshot.builder         = builder;
    if (builder) {
        snapshot.flags = 1;
        if (builder->initialized()) {
            snapshot.flags |= 2;
            if (builder->enabled()) snapshot.flags |= 4;
            if (builder->isDeferredCapable()) snapshot.flags |= 8;
            if (builder->isDeferredEnabled()) snapshot.flags |= 16;
            if (builder->isRayTracingCapable()) snapshot.flags |= 32;
            if (builder->isRayTracingEnabled()) snapshot.flags |= 64;
            if (builder->isUpscalingAvailable()) snapshot.flags |= 128;
            if (builder->isUpscalingEnabled()) snapshot.flags |= 256;
            auto const resolution  = builder->getRenderResolution();
            snapshot.width         = resolution.x;
            snapshot.height        = resolution.y;
            snapshot.upscaleFactor = builder->getUpscalingFactor();
        }
    }
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::RenderState,
        builder,
        client,
        static_cast<uint64_t>(stage),
        snapshot.flags,
        (snapshot.width << 32) | snapshot.height,
        std::bit_cast<uint32_t>(snapshot.upscaleFactor)
    );

    auto&                         logger = diagnosticLogger();
    thread_local RendererSnapshot previous;
    thread_local uint64_t         previousSession{};
    bool const                    modeChanged =
        previousSession != session || snapshot.builder != previous.builder || snapshot.flags != previous.flags;
    if (modeChanged) ++gClientSnapshotRequest;
    if (!periodic || modeChanged) {
        previous        = snapshot;
        previousSession = session;
        logger.info(
            "[RenderDiag] session={} afterEvent={} stage={} thread={} builder={} initialized={} enabled={} "
            "deferred={}/{} rtx={}/{} upscaling={}/{} render={}x{} factor={} offline={}",
            session,
            afterEvent,
            stageName(stage),
            GetCurrentThreadId(),
            snapshot.builder,
            (snapshot.flags & 2) != 0,
            (snapshot.flags & 4) != 0,
            (snapshot.flags & 16) != 0,
            (snapshot.flags & 8) != 0,
            (snapshot.flags & 64) != 0,
            (snapshot.flags & 32) != 0,
            (snapshot.flags & 256) != 0,
            (snapshot.flags & 128) != 0,
            snapshot.width,
            snapshot.height,
            snapshot.upscaleFactor,
            isOfflineRenderActivityActive()
        );
    }
    if (graphics || !client) return;
    auto&       replay = replay::ReplaySession::getInstance();
    auto const& size   = *client->getViewportInfo().size;
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::ClientRenderState,
        client,
        nullptr,
        static_cast<uint64_t>(stage),
        std::bit_cast<uint32_t>(size.x),
        std::bit_cast<uint32_t>(size.y),
        static_cast<uint64_t>(replay.getCurrentTick())
    );
    auto&                    packs       = client->getResourcePackManager();
    bool const               vvSupported = packs.supportsVibrantVisuals();
    bool const               pbr         = packs.hasCapability("pbr");
    bool const               raytraced   = packs.hasCapability("raytraced");
    bool const               loaded      = packs.areGameplayResourcesLoaded();
    std::vector<std::string> entries;
    packs.iteratePacks([&](PackInstance const& pack) {
        auto const  id       = pack.getPackId();
        auto const& manifest = pack.getManifest();
        entries.push_back(
            fmt::format(
                "id={:016x}:{:016x} version={:?} origin={} subpack={:?} name={:?} pbr={} raytraced={} slice={}",
                id.a,
                id.b,
                std::string_view(pack.getVersion().asString()).substr(0, 128),
                static_cast<int>(pack.getPackOrigin()),
                std::string_view(pack.getSubpackFolderName()).substr(0, 128),
                std::string_view(manifest.getName()).substr(0, 128),
                manifest.hasPackCapability("pbr"),
                manifest.hasPackCapability("raytraced"),
                pack.isSlicePack()
            )
        );
    });
    auto const packCount = entries.size();
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::ResourceState,
        &packs,
        client,
        static_cast<uint64_t>(stage),
        (vvSupported ? 1ull : 0ull) | (pbr ? 2ull : 0ull) | (raytraced ? 4ull : 0ull),
        loaded,
        packCount
    );
    thread_local uint64_t                 packSession{};
    thread_local ClientInstance*          packClient{};
    thread_local std::vector<std::string> previousEntries;
    thread_local std::string              previousSummary;
    thread_local uint64_t                 stackRevision{};
    bool const stackChanged = packSession != session || packClient != client || entries != previousEntries;
    if (stackChanged) {
        packSession     = session;
        packClient      = client;
        previousEntries = std::move(entries);
        ++stackRevision;
    }
    auto summary = fmt::format(
        "active={} joined={} worldReady={} paused={} packVVSupported={} packPbr={} packRaytraced={} resourcesLoaded={}",
        replay.isActive(),
        replay.hasJoinedReplayWorld(),
        replay.isReplayWorldReady(),
        replay.isPaused(),
        vvSupported,
        pbr,
        raytraced,
        loaded
    );
    thread_local uint64_t lastSummaryEvent{};
    if (!periodic || requestChanged || modeChanged || stackChanged || summary != previousSummary
        || afterEvent != lastSummaryEvent) {
        previousSummary  = summary;
        lastSummaryEvent = afterEvent;
        logger.info(
            "[RenderDiag] session={} afterEvent={} stage={} thread={} clientSnapshot=true viewport={}x{} tick={} "
            "stackRevision={} packCount={} {}",
            session,
            afterEvent,
            stageName(stage),
            GetCurrentThreadId(),
            size.x,
            size.y,
            replay.getCurrentTick(),
            stackRevision,
            packCount,
            summary
        );
    }
    if (stackChanged) {
        for (size_t i = 0; i < previousEntries.size(); ++i) {
            logger.info(
                "[RenderDiag] session={} stackRevision={} packIndex={}/{} {}",
                session,
                stackRevision,
                i + 1,
                previousEntries.size(),
                previousEntries[i]
            );
        }
    }
} catch (...) {
    reportDiagnosticFailure();
}

uint64_t recordReplayAdmissionBoundary(std::string_view phase, uint64_t span) noexcept try {
    if (!renderDiagnosticsEnabled()) return 0;
    auto const event = ++gAdmissionSequence;
    ++gClientSnapshotRequest;
    diagnosticLogger().info(
        "[ReplayAdmission] session={} event={} span={} thread={} phase={}",
        gAdmissionSession.load(),
        event,
        span ? span : event,
        GetCurrentThreadId(),
        phase
    );
    return event;
} catch (...) {
    reportDiagnosticFailure();
    return 0;
}

void recordReplayPacksInfo(
    std::string_view               source,
    ResourcePacksInfoPacket const& packet,
    bool                           selected,
    uint64_t                       span
) noexcept try {
    if (!renderDiagnosticsEnabled()) return;
    auto const& data   = *packet.mData;
    auto&       logger = diagnosticLogger();
    logger.info(
        "[ReplayAdmission] session={} span={} thread={} packet=Info source={} selected={} "
        "forceDisableVibrantVisuals={} required={} addons={} scripts={} packs={}",
        gAdmissionSession.load(),
        span,
        GetCurrentThreadId(),
        source,
        selected,
        data.mForceDisableVibrantVisuals,
        data.mResourcePackRequired,
        data.mHasAddonPacks,
        data.mHasScripts,
        data.mResourcePacks->size()
    );
    size_t index{};
    for (auto const& pack : *data.mResourcePacks) {
        auto const& id = *pack.mPackIdVersion->mId;
        logger.info(
            "[ReplayAdmission] span={} packet=Info source={} index={} id={:016x}:{:016x} version={:?} "
            "subpack={:?} addon={} scripts={} rayTracingCapable={} exceptions={}",
            span,
            source,
            ++index,
            id.a,
            id.b,
            std::string_view(pack.mPackIdVersion->mVersion->asString()).substr(0, 128),
            std::string_view(*pack.mSubpackName).substr(0, 128),
            pack.mIsAddonPack,
            pack.mHasScripts,
            pack.mIsRayTracingCapable,
            pack.mHasExceptions
        );
    }
} catch (...) {
    reportDiagnosticFailure();
}

void recordReplayPackStack(
    std::string_view               source,
    ResourcePackStackPacket const& packet,
    bool                           selected,
    uint64_t                       span
) noexcept try {
    if (!renderDiagnosticsEnabled()) return;
    auto&       logger      = diagnosticLogger();
    auto const& experiments = *packet.mExperiments;
    logger.info(
        "[ReplayAdmission] session={} span={} thread={} packet=Stack source={} selected={} required={} "
        "editorPacks={} packs={} baseVersion={:?} experiments={} deprecated={} everToggled={}",
        gAdmissionSession.load(),
        span,
        GetCurrentThreadId(),
        source,
        selected,
        packet.mTexturePackRequired,
        packet.mIncludeEditorPacks,
        packet.mTexturePackIdsAndVersions->size(),
        packet.mBaseGameVersion->asString(),
        experimentBits(*experiments.mExperimentData),
        experimentBits(*experiments.mDeprecatedData),
        experiments.mExperimentsEverToggled
    );
    size_t index{};
    for (auto const& pack : *packet.mTexturePackIdsAndVersions) {
        auto const& id = *pack.mPackId->mId;
        logger.info(
            "[ReplayAdmission] span={} packet=Stack source={} index={} id={:016x}:{:016x} version={:?} subpack={:?}",
            span,
            source,
            ++index,
            id.a,
            id.b,
            std::string_view(pack.mPackId->mVersion->asString()).substr(0, 128),
            std::string_view(*pack.mSubpackName).substr(0, 128)
        );
    }
} catch (...) {
    reportDiagnosticFailure();
}

void recordReplayWorldSettings(std::string_view source, LevelSettings const& settings, uint64_t span) noexcept try {
    if (!renderDiagnosticsEnabled()) return;
    auto const& experiments = *settings.mExperiments;
    diagnosticLogger().info(
        "[ReplayAdmission] session={} span={} thread={} worldSettings={} gameType={} generator={} "
        "editorWorldType={} immutable={} textureRequired={} lockedResourcePack={} lockedTemplate={} "
        "fromTemplate={} templateOptionsLocked={} overrideSettings={} resourcePacks={} behaviorPacks={} "
        "baseVersion={:?} experiments={} deprecated={} everToggled={}",
        gAdmissionSession.load(),
        span,
        GetCurrentThreadId(),
        source,
        static_cast<int>(settings.mGameType),
        static_cast<int>(settings.mGenerator),
        static_cast<int>(settings.mEditorWorldType),
        settings.mImmutableWorld,
        settings.mTexturePacksRequired,
        settings.mHasLockedResourcePack,
        settings.mIsFromLockedTemplate,
        settings.mIsFromWorldTemplate,
        settings.mIsWorldTemplateOptionLocked,
        settings.mOverrideSettings,
        settings.mNewWorldResourcePackIdentities->size(),
        settings.mNewWorldBehaviorPackIdentities->size(),
        settings.mBaseGameVersion->asString(),
        experimentBits(*experiments.mExperimentData),
        experimentBits(*experiments.mDeprecatedData),
        experiments.mExperimentsEverToggled
    );
} catch (...) {
    reportDiagnosticFailure();
}

void recordReplayRequestedGraphicsMode(MinecraftScreenModel const& model) noexcept try {
    if (!renderDiagnosticsEnabled()) return;
    diagnosticLogger().info(
        "[ReplayAdmission] session={} thread={} requestedGraphicsMode={} source=MinecraftScreenModel",
        gAdmissionSession.load(),
        GetCurrentThreadId(),
        model.getGraphicsMode()
    );
} catch (...) {
    reportDiagnosticFailure();
}

} // namespace playback::exporting
