#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace playback::runtime {

// Game functions the 26.60 SDK declares but does not export.
enum class GameFunction : uint8_t {
    HasFinishedLoading,
    ResourceLocationGetFullPath,
    UpdateAttributesPacketConstructor,
    UpdateAttributesPacketPayloadConstructor,
    MapGetFullDataPacket,
    GetCameraPosition,
    UpdateAttribute,
    AttributeDataConstructor,
    AttributeDataDestructor,
    MapDataApplyToMap,
    NetworkItemStackDescriptorGetItemStack,
    PlayerIsScoping,
    HudShowSurvivalUi,
    PlayPredictiveSynchronizedSound,
    ActorRenderDispatcherRender,
    HudScreenTick,
    ContainerScreenIsStillValid,
    RenderItemInHand,
    ShouldRenderFirstPersonObjects,
    RenderTransparentFirstPerson3DObjects,
    GetCameraTargetActor,
    HudVignetteRender,
    TryGetPortalData,
    ActorIsOnFire,
    ClientOnTick,
    BgfxD3D12Flip,
    BgfxD3D11Flip,
    BgfxD3D11PreReset,
    BgfxD3D11PostReset,
    BgfxContextInitFinalize,
    BgfxContextShutdownBegin,
    DataDrivenModelSetupAttachableNoChecks,
    WeatherTick,
    WeatherGetTopSnowBlockPos,
    LevelChunkGetTopRainBlockPos,
    LevelRendererPlayerTickRain,
    BiomeRegistryGetBiomesInDimension,
    GameRulesSetGameRule,
    LevelDataSetDaylightCycle,
    Count,
};

// The function's address in the supported game build, or 0 for every function in any other build. The network hooks
// need some of them, so Playback does not load there and direct calls need no null check.
[[nodiscard]] uintptr_t gameAddress(GameFunction function);

template <class Fn>
    requires std::is_function_v<std::remove_pointer_t<Fn>>
[[nodiscard]] Fn gameFunction(GameFunction function) {
    return reinterpret_cast<Fn>(gameAddress(function));
}

// Members must be called as members: a class return goes through a hidden pointer after `this`.
template <class Fn>
    requires(std::is_member_function_pointer_v<Fn> && sizeof(Fn) == sizeof(uintptr_t))
[[nodiscard]] Fn gameMember(GameFunction function) {
    return std::bit_cast<Fn>(gameAddress(function));
}

} // namespace playback::runtime
