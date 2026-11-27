#include "ReplayWorldEnvHooks.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/common/SubClientId.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/minecraft_renderer/objects/SkyRenderObject.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/minecraft_renderer/objects/WeatherRenderObject.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Weather.h"
#include "mc/world/level/WeatherHelpers.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/biome/registry/BiomeRegistry.h"
#include "mc/world/level/dimension/Dimension.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace playback::visuals {

namespace {

std::atomic<bool>    gActive{false};
std::atomic<bool>    gRecordedTimeValid{false};
std::atomic<int>     gRecordedTimeTicks{0};
std::atomic<bool>    gDaylightCycleRunning{false};
std::atomic<float>   gPartialTick{0.0f};
std::atomic<bool>    gTimeOverrideEnabled{false};
std::atomic<double>  gTimeOverrideTicks{0.0};
std::atomic<uint8_t> gWeatherMode{static_cast<uint8_t>(ReplayWeatherMode::Recorded)};

thread_local bool         tInTickRain        = false;
thread_local Biome const* tRainFallbackBiome = nullptr;

[[nodiscard]] ReplayWeatherMode activeWeatherMode() noexcept {
    return static_cast<ReplayWeatherMode>(gWeatherMode.load(std::memory_order_acquire));
}

[[nodiscard]] bool isRainSuppressedMode(ReplayWeatherMode mode) noexcept {
    return mode == ReplayWeatherMode::Clear || mode == ReplayWeatherMode::Overcast || mode == ReplayWeatherMode::Snow;
}

[[nodiscard]] float targetFogLevelForMode(ReplayWeatherMode mode) noexcept {
    return mode == ReplayWeatherMode::Clear ? 0.0f : 1.0f;
}

[[nodiscard]] Biome const* findRainCapableBiome(Level const& level) noexcept {
    auto const& registry = level.getBiomeRegistry();
    if (auto const* plains = registry.lookupByName("plains"); plains && plains->mRain) {
        return plains;
    }
    if (auto const* forest = registry.lookupByName("forest"); forest && forest->mRain) {
        return forest;
    }
    for (auto const* biome : registry.getBiomesInDimension(DimensionType{0})) {
        if (biome && biome->mRain) return biome;
    }
    return nullptr;
}

// Overrides dimension time of day during replay with either user override or recorded tick state.
LL_TYPE_INSTANCE_HOOK(
    DimensionGetTimeOfDayHook,
    ll::memory::HookPriority::Normal,
    Dimension,
    &Dimension::$getTimeOfDay,
    float,
    int   time,
    float a
) {
    if (gActive.load(std::memory_order_acquire)) {
        if (gTimeOverrideEnabled.load(std::memory_order_acquire)) {
            double const val    = gTimeOverrideTicks.load(std::memory_order_relaxed);
            double const wholeD = std::floor(val);
            float const  frac   = std::clamp(static_cast<float>(val - wholeD), 0.0f, 1.0f);
            int const    whole  = static_cast<int>(((static_cast<int64_t>(wholeD) % 24000) + 24000) % 24000);
            return origin(whole, frac);
        }
        if (gRecordedTimeValid.load(std::memory_order_acquire)) {
            int const   whole = gRecordedTimeTicks.load(std::memory_order_relaxed);
            float const frac  = gDaylightCycleRunning.load(std::memory_order_acquire)
                                  ? std::clamp(gPartialTick.load(std::memory_order_relaxed), 0.0f, 1.0f)
                                  : 0.0f;
            return origin(whole, frac);
        }
    }
    return origin(time, a);
}

LL_TYPE_INSTANCE_HOOK(
    WeatherRainLevelHook,
    ll::memory::HookPriority::Normal,
    Weather,
    &Weather::getRainLevel,
    float,
    float a
) {
    if (gActive.load(std::memory_order_acquire)) {
        auto const mode = activeWeatherMode();
        if (mode != ReplayWeatherMode::Recorded) return mode == ReplayWeatherMode::Clear ? 0.0f : 1.0f;
    }
    return origin(a);
}

LL_TYPE_INSTANCE_HOOK(
    WeatherLightningLevelHook,
    ll::memory::HookPriority::Normal,
    Weather,
    &Weather::getLightningLevel,
    float,
    float a
) {
    if (gActive.load(std::memory_order_acquire)) {
        auto const mode = activeWeatherMode();
        if (mode != ReplayWeatherMode::Recorded) return mode == ReplayWeatherMode::Thunder ? 1.0f : 0.0f;
    }
    return origin(a);
}

LL_TYPE_INSTANCE_HOOK(WeatherIsRainingHook, ll::memory::HookPriority::Normal, Weather, &Weather::isRaining, bool) {
    if (gActive.load(std::memory_order_acquire)) {
        auto const mode = activeWeatherMode();
        if (mode != ReplayWeatherMode::Recorded) return mode != ReplayWeatherMode::Clear;
    }
    return origin();
}

LL_TYPE_INSTANCE_HOOK(WeatherIsLightningHook, ll::memory::HookPriority::Normal, Weather, &Weather::isLightning, bool) {
    if (gActive.load(std::memory_order_acquire)) {
        auto const mode = activeWeatherMode();
        if (mode != ReplayWeatherMode::Recorded) return mode == ReplayWeatherMode::Thunder;
    }
    return origin();
}

LL_STATIC_HOOK(
    WeatherShouldSnowHook,
    ll::memory::HookPriority::Normal,
    &WeatherHelpers::shouldSnow,
    bool,
    ::BlockSource&    region,
    ::BlockPos const& pos
) {
    if (gActive.load(std::memory_order_acquire)) {
        auto const mode = activeWeatherMode();
        if (mode == ReplayWeatherMode::Snow) return true;
        if (mode == ReplayWeatherMode::Rain || mode == ReplayWeatherMode::Thunder) return false;
    }
    return origin(region, pos);
}

LL_TYPE_INSTANCE_HOOK(
    BiomeCanHaveSnowfallHook,
    ll::memory::HookPriority::Normal,
    Biome,
    &Biome::canHaveSnowfall,
    bool,
    ::BlockSource const& region,
    ::BlockPos const&    pos
) {
    if (gActive.load(std::memory_order_acquire)) {
        auto const mode = activeWeatherMode();
        if (mode == ReplayWeatherMode::Snow) return true;
        if (mode == ReplayWeatherMode::Rain || mode == ReplayWeatherMode::Thunder) return false;
    }
    return origin(region, pos);
}

LL_STATIC_HOOK(
    WeatherGetTopSnowBlockPosHook,
    ll::memory::HookPriority::Normal,
    &WeatherHelpers::getTopSnowBlockPos,
    ::BlockPos const,
    ::BlockSource&    region,
    ::BlockPos const& pos
) {
    if (gActive.load(std::memory_order_acquire) && activeWeatherMode() == ReplayWeatherMode::Snow) {
        if (auto* chunk = region.getChunkAt(pos)) {
            return WeatherHelpers::getTopRainBlockPos(chunk, pos);
        }
    }
    return origin(region, pos);
}

LL_TYPE_INSTANCE_HOOK(
    BlockSourceGetBiomeHook,
    ll::memory::HookPriority::Normal,
    BlockSource,
    &BlockSource::getBiome,
    ::Biome const&,
    ::BlockPos const& pos
) {
    if (!tInTickRain) [[likely]]
        return origin(pos);
    auto const& biome = origin(pos);
    if (!biome.mRain && tRainFallbackBiome != nullptr) {
        return *tRainFallbackBiome;
    }
    return biome;
}

LL_TYPE_INSTANCE_HOOK(
    BiomeGetTemperatureHook,
    ll::memory::HookPriority::Normal,
    Biome,
    &Biome::getTemperature,
    float,
    ::BlockSource const& region,
    ::BlockPos const&    pos
) {
    if (!tInTickRain) [[likely]]
        return origin(region, pos);
    auto const mode = activeWeatherMode();
    if (mode == ReplayWeatherMode::Snow) return 0.0f;
    if (mode == ReplayWeatherMode::Rain || mode == ReplayWeatherMode::Thunder) return 0.8f;
    return origin(region, pos);
}

// Drives precipitation updates across dry/warm biomes and enforces immediate sky/fog levels.
LL_TYPE_INSTANCE_HOOK(
    LevelRendererPlayerTickRainHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::$tickRain,
    void
) {
    if (!gActive.load(std::memory_order_acquire)) {
        origin();
        return;
    }
    auto const mode = activeWeatherMode();
    if (mode == ReplayWeatherMode::Recorded) {
        origin();
        return;
    }

    auto syncWeatherFog = [this, mode]() noexcept {
        if (auto* dim = this->mDimension; dim && dim->mHasWeather && dim->mWeather.get() != nullptr) {
            dim->getWeather().mFogLevel = targetFogLevelForMode(mode);
        }
    };

    if (mode == ReplayWeatherMode::Clear || mode == ReplayWeatherMode::Overcast) {
        syncWeatherFog();
        return;
    }

    struct TickRainScope {
        bool         prevInTickRain;
        Biome const* prevFallback;
        explicit TickRainScope(Biome const* fallback) noexcept
        : prevInTickRain(tInTickRain),
          prevFallback(tRainFallbackBiome) {
            tInTickRain        = true;
            tRainFallbackBiome = fallback;
        }
        ~TickRainScope() noexcept {
            tInTickRain        = prevInTickRain;
            tRainFallbackBiome = prevFallback;
        }
    };

    Biome const* fallback = nullptr;
    if (auto* dim = this->mDimension; dim && dim->mHasWeather) {
        fallback = findRainCapableBiome(this->mLevel);
    }
    {
        TickRainScope scope{fallback};
        origin();
    }
    syncWeatherFog();
}

// Clears rain sample counts and splash particles for Clear, Overcast, and Snow overrides.
LL_TYPE_INSTANCE_HOOK(
    LevelRendererCameraCalcRainStateHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::calcRainState,
    ::LevelRendererCamera::RainState,
    ::Vec3 const& rainPos
) {
    auto state = origin(rainPos);
    if (gActive.load(std::memory_order_acquire) && isRainSuppressedMode(activeWeatherMode())) {
        state.mRainCount      = 0;
        state.mRainPosSamples = 0;
        state.mRainSplashParticles->clear();
        state.mSmokeParticles->clear();
    }
    return state;
}

// Adjusts extracted view weather densities, occlusion texture, and sky fog level for the active override mode.
LL_TYPE_INSTANCE_HOOK(
    LevelRendererPlayerCreateViewHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::createViewRenderObject,
    ::ViewRenderObject,
    ::ScreenContext& screenContext,
    ::SubClientId    clientSubId
) {
    bool const        active = gActive.load(std::memory_order_acquire);
    ReplayWeatherMode mode   = ReplayWeatherMode::Recorded;
    if (active) {
        mode = activeWeatherMode();
        if (mode != ReplayWeatherMode::Recorded) {
            if (auto* dim = this->mDimension; dim && dim->mHasWeather && dim->mWeather.get() != nullptr) {
                dim->getWeather().mFogLevel = targetFogLevelForMode(mode);
            }
        }
    }
    auto view = origin(screenContext, clientSubId);
    if (active && mode != ReplayWeatherMode::Recorded) {
        view.mSkyState->mFogLevel = targetFogLevelForMode(mode);
        switch (mode) {
        case ReplayWeatherMode::Clear:
        case ReplayWeatherMode::Overcast:
            view.mWeatherState->mDensityRain   = 0.0f;
            view.mWeatherState->mDensitySnow   = 0.0f;
            view.mWeatherState->mParticleCount = 0;
            break;
        case ReplayWeatherMode::Snow: {
            float const density = std::max(
                1.0f,
                std::max(
                    static_cast<float>(view.mWeatherState->mDensitySnow),
                    static_cast<float>(view.mWeatherState->mDensityRain)
                )
            );
            view.mWeatherState->mDensitySnow = density;
            view.mWeatherState->mDensityRain = 0.0f;
            if (view.mWeatherState->mWeatherOcclusionTexture->mResourcePointerBlock) {
                view.mWeatherState->mWeatherSnowOcclusionTexture->mResourcePointerBlock =
                    view.mWeatherState->mWeatherOcclusionTexture->mResourcePointerBlock;
            }
            break;
        }
        case ReplayWeatherMode::Rain:
        case ReplayWeatherMode::Thunder: {
            float const density = std::max(
                1.0f,
                std::max(
                    static_cast<float>(view.mWeatherState->mDensityRain),
                    static_cast<float>(view.mWeatherState->mDensitySnow)
                )
            );
            view.mWeatherState->mDensityRain = density;
            view.mWeatherState->mDensitySnow = 0.0f;
            break;
        }
        case ReplayWeatherMode::Recorded:
            break;
        }
    }
    return view;
}

struct HookSlot {
    char const* name;
    bool (*hook)();
    bool (*unhook)();
    bool ok;
};

template <class H>
HookSlot makeSlot(char const* name) {
    return HookSlot{name, [] { return H::hook() == 0; }, [] { return H::unhook(); }, false};
}

} // namespace

bool hookReplayWorldEnv(bool enable) {
    static bool                  installed = false;
    static bool                  complete  = false;
    static std::vector<HookSlot> slots;

    if (enable) {
        if (installed) return complete;
        slots = {
            makeSlot<DimensionGetTimeOfDayHook>("Dimension::$getTimeOfDay"),
            makeSlot<WeatherRainLevelHook>("Weather::getRainLevel"),
            makeSlot<WeatherLightningLevelHook>("Weather::getLightningLevel"),
            makeSlot<WeatherIsRainingHook>("Weather::isRaining"),
            makeSlot<WeatherIsLightningHook>("Weather::isLightning"),
            makeSlot<WeatherShouldSnowHook>("WeatherHelpers::shouldSnow"),
            makeSlot<WeatherGetTopSnowBlockPosHook>("WeatherHelpers::getTopSnowBlockPos"),
            makeSlot<BiomeCanHaveSnowfallHook>("Biome::canHaveSnowfall"),
            makeSlot<BlockSourceGetBiomeHook>("BlockSource::getBiome"),
            makeSlot<BiomeGetTemperatureHook>("Biome::getTemperature"),
            makeSlot<LevelRendererPlayerTickRainHook>("LevelRendererPlayer::$tickRain"),
            makeSlot<LevelRendererCameraCalcRainStateHook>("LevelRendererCamera::calcRainState"),
            makeSlot<LevelRendererPlayerCreateViewHook>("LevelRendererPlayer::createViewRenderObject"),
        };
        complete = true;
        for (auto& slot : slots) {
            slot.ok   = slot.hook();
            complete &= slot.ok;
        }
        installed = true;
        if (!complete) {
            std::string missing;
            for (auto const& slot : slots) {
                if (!slot.ok) missing += std::string(" ") + slot.name;
            }
            Playback::getInstance().getSelf().getLogger().warn("Replay world env hooks incomplete:{}", missing);
        }
        return complete;
    }

    if (!installed) return true;
    bool allUnhooked = true;
    for (auto& slot : slots) {
        if (slot.ok) {
            if (slot.unhook()) slot.ok = false;
            else allUnhooked = false;
        }
    }
    if (allUnhooked) slots.clear();
    installed = !allUnhooked;
    complete  = false;
    setReplayWorldEnvActive(false);
    return allUnhooked;
}

void setReplayWorldEnvActive(bool active) noexcept {
    if (!active) {
        gActive.store(false, std::memory_order_release);
        gRecordedTimeValid.store(false, std::memory_order_release);
        gTimeOverrideEnabled.store(false, std::memory_order_release);
        gWeatherMode.store(static_cast<uint8_t>(ReplayWeatherMode::Recorded), std::memory_order_release);
        return;
    }
    gActive.store(true, std::memory_order_release);
}

void setReplayWorldTimeState(int recordedTimeTicks, bool daylightCycleRunning, float partialTick) noexcept {
    gRecordedTimeTicks.store(recordedTimeTicks, std::memory_order_relaxed);
    gDaylightCycleRunning.store(daylightCycleRunning, std::memory_order_relaxed);
    gPartialTick.store(partialTick, std::memory_order_relaxed);
    gRecordedTimeValid.store(true, std::memory_order_release);
}

void setReplayWorldTimeOverride(std::optional<double> overrideTimeTicks) noexcept {
    if (overrideTimeTicks.has_value()) {
        gTimeOverrideTicks.store(*overrideTimeTicks, std::memory_order_relaxed);
        gTimeOverrideEnabled.store(true, std::memory_order_release);
    } else {
        gTimeOverrideEnabled.store(false, std::memory_order_release);
    }
}

void setReplayWeatherOverride(ReplayWeatherMode mode) noexcept {
    gWeatherMode.store(static_cast<uint8_t>(mode), std::memory_order_release);
}

ReplayWeatherMode getReplayWeatherOverride() noexcept {
    return static_cast<ReplayWeatherMode>(gWeatherMode.load(std::memory_order_acquire));
}

} // namespace playback::visuals
