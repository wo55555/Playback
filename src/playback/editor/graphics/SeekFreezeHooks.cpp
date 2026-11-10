#include "SeekFreezeHooks.h"

#include "playback/Playback.h"
#include "playback/editor/input/EditorInput.h"
#include "playback/exporting/ExportActivity.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

// The 26.10 I18nObserver template needs this complete before MinecraftGame's headers.
#include "mc/platform/threading/Mutex.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/util/Timer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace playback::editor::graphics {

namespace {

// A seek that never lands must not hide the world for good.
constexpr auto MaxFreezeDuration = std::chrono::seconds(30);
// bgfx presents a frame after the main thread submits it, so the first resumed presents can still be empty.
constexpr uint32_t ResumeGuardFrames = 3;

enum class FreezeEnd : uint8_t { SeekDone, Dimension, Export, Inactive, Timeout };

std::string_view freezeEndName(FreezeEnd end) {
    switch (end) {
    case FreezeEnd::SeekDone:
        return "seekDone";
    case FreezeEnd::Dimension:
        return "dimension";
    case FreezeEnd::Export:
        return "export";
    case FreezeEnd::Inactive:
        return "inactive";
    case FreezeEnd::Timeout:
        return "timeout";
    }
    return "unknown";
}

struct FreezeEpisode {
    std::chrono::steady_clock::time_point startedAt;
    uint64_t                              skippedFrames{};
};

// Main thread only: updateGraphics and ReplaySession::tick both run there.
std::optional<FreezeEpisode> gEpisode;
bool                         gTimedOut{};

std::atomic<bool>     gFrozen{};
std::atomic<uint32_t> gFramesSinceResume{ResumeGuardFrames};

std::optional<FreezeEnd> freezeBlocker(replay::ReplaySession const& replay) {
    if (!replay.isActive() || !replay.hasJoinedReplayWorld()) return FreezeEnd::Inactive;
    // Export seeks run under the offline clock, which already decides when graphics update.
    if (exporting::isExportActivityActive() || exporting::isOfflineRenderActivityActive()) return FreezeEnd::Export;
    if (!replay.isSeekPending()) return FreezeEnd::SeekDone;
    // The native dimension loading screen needs graphics to advance.
    if (replay.isDimensionTransitionPending()) return FreezeEnd::Dimension;
    return std::nullopt;
}

void endEpisode(FreezeEnd end, replay::ReplaySession const& replay) {
    auto const ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gEpisode->startedAt)
            .count();
    auto& logger = Playback::getInstance().getSelf().getLogger();
    if (end == FreezeEnd::Timeout) {
        logger.warn(
            "Seek freeze ended (reason=timeout, skippedFrames={}, durationMs={}, replayTick={}, target={})",
            gEpisode->skippedFrames,
            ms,
            replay.getAppliedReplayTick(),
            replay.getCurrentTick()
        );
    } else {
        logger.info(
            "Seek freeze ended (reason={}, skippedFrames={}, durationMs={}, replayTick={})",
            freezeEndName(end),
            gEpisode->skippedFrames,
            ms,
            replay.getAppliedReplayTick()
        );
    }
    gEpisode.reset();
    gFramesSinceResume.store(0, std::memory_order_release);
    gFrozen.store(false, std::memory_order_release);
}

LL_TYPE_INSTANCE_HOOK(
    SeekFreezeUpdateGraphicsHook,
    ll::memory::HookPriority::Normal,
    MinecraftGame,
    &MinecraftGame::updateGraphics,
    void,
    Bedrock::NotNullNonOwnerPtr<IClientInstance> const& client,
    Timer const&                                        timer
) {
    auto&      replay = replay::ReplaySession::getInstance();
    auto const end    = freezeBlocker(replay);
    if (end == FreezeEnd::SeekDone || end == FreezeEnd::Inactive) gTimedOut = false;

    if (!end && !gTimedOut) {
        auto const now = std::chrono::steady_clock::now();
        if (!gEpisode) {
            gEpisode = FreezeEpisode{now};
            gFrozen.store(true, std::memory_order_release);
            // Without the editor the swap chain shows whatever bgfx presents, so the freeze is only held in the
            // viewport.
            Playback::getInstance().getSelf().getLogger().info(
                "Seek freeze began (replayTick={}, target={}, editorUi={})",
                replay.getAppliedReplayTick(),
                replay.getCurrentTick(),
                input::isUiVisible()
            );
        }
        if (now - gEpisode->startedAt < MaxFreezeDuration) {
            ++gEpisode->skippedFrames;
            return;
        }
        gTimedOut = true;
        endEpisode(FreezeEnd::Timeout, replay);
    } else if (gEpisode) {
        endEpisode(end.value_or(FreezeEnd::Timeout), replay);
    }

    origin(client, timer);
    auto const resumed = gFramesSinceResume.load(std::memory_order_acquire);
    if (resumed < ResumeGuardFrames) gFramesSinceResume.store(resumed + 1, std::memory_order_release);
}

} // namespace

bool hookSeekFreeze(bool enable) {
    static bool installed{};
    if (enable) {
        if (!installed) installed = SeekFreezeUpdateGraphicsHook::hook() == 0;
        Playback::getInstance().getSelf().getLogger().info("Seek freeze hooks: updateGraphics={}", installed);
        return installed;
    }
    if (installed && SeekFreezeUpdateGraphicsHook::unhook()) installed = false;
    gEpisode.reset();
    gTimedOut = false;
    gFrozen.store(false, std::memory_order_release);
    gFramesSinceResume.store(ResumeGuardFrames, std::memory_order_release);
    return !installed;
}

bool isSeekDisplayFrozen() noexcept {
    return gFrozen.load(std::memory_order_acquire)
        || gFramesSinceResume.load(std::memory_order_acquire) < ResumeGuardFrames;
}

} // namespace playback::editor::graphics
