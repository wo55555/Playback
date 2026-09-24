#pragma once

#include <cstdint>
#include <string_view>

class ClientInstance;
class Level;
class Dimension;
class LevelSettings;
class MinecraftScreenModel;
class ResourcePacksInfoPacket;
class ResourcePackStackPacket;

namespace playback::exporting {

enum class RenderDiagnosticStage : uint64_t {
    ReplayStart = 1,
    ReplayPacksPrepared,
    ReplayWorldJoined,
    ReplayWorldReady,
    EditorShown,
    EditorHidden,
    ClientPulse,
    ExportBeforeOpen,
    ExportAfterOpen,
    ResizeBefore,
    ResizeAfter,
    RestoreBefore,
    RestoreAfter,
    GraphicsBefore,
    GraphicsAfter,
    ExportClosed,
};

[[nodiscard]] bool renderDiagnosticsEnabled() noexcept;

// Only a ray-traced pipeline accumulates across frames, so raster shaders need no convergence passes at all.
[[nodiscard]] bool rayTracingActive() noexcept;

enum class RenderDiagnosticProbe { None, ViewInputs, Clock, NativeSubmit, CaptureLineage, SceneCorrespondence };

struct RenderDiagnosticProfile {
    bool                         enabled{};
    bool                         known{};
    RenderDiagnosticProbe        probe{};
    [[nodiscard]] constexpr bool nativeSubmit() const noexcept {
        return enabled && probe == RenderDiagnosticProbe::NativeSubmit;
    }
    [[nodiscard]] constexpr bool captureLineage() const noexcept {
        return enabled && probe == RenderDiagnosticProbe::CaptureLineage;
    }
    [[nodiscard]] constexpr bool sceneCorrespondence() const noexcept {
        return enabled && probe == RenderDiagnosticProbe::SceneCorrespondence;
    }
    [[nodiscard]] constexpr bool submitScope() const noexcept {
        return nativeSubmit() || captureLineage() || sceneCorrespondence();
    }
    [[nodiscard]] constexpr bool extractFrame() const noexcept { return enabled; }
    [[nodiscard]] constexpr bool endFrame() const noexcept { return enabled && probe == RenderDiagnosticProbe::Clock; }
    [[nodiscard]] constexpr uint32_t hookMask() const noexcept {
        return (extractFrame() ? 1u : 0u) | (endFrame() ? 2u : 0u);
    }
    [[nodiscard]] constexpr uint32_t submitHookMask() const noexcept { return submitScope() ? 3u : 0u; }
};

[[nodiscard]] constexpr RenderDiagnosticProfile
selectRenderDiagnosticProfile(bool enabled, std::string_view name) noexcept {
    auto const probe = name == "view-inputs"          ? RenderDiagnosticProbe::ViewInputs
                     : name == "clock"                ? RenderDiagnosticProbe::Clock
                     : name == "native-submit"        ? RenderDiagnosticProbe::NativeSubmit
                     : name == "capture-lineage"      ? RenderDiagnosticProbe::CaptureLineage
                     : name == "scene-correspondence" ? RenderDiagnosticProbe::SceneCorrespondence
                                                      : RenderDiagnosticProbe::None;
    enabled          = enabled && name != "off";
    return {
        enabled,
        name == "off" || name == "observe" || probe != RenderDiagnosticProbe::None,
        enabled ? probe : RenderDiagnosticProbe::None
    };
}

[[nodiscard]] RenderDiagnosticProfile renderDiagnosticProfile() noexcept;
enum class WorldDiagnosticStage : uint64_t {
    ReplayTickBefore = 1,
    NativeTickBefore,
    NativeTickAfter,
    GraphicsBefore,
    GraphicsAfter,
    CameraSetup,
};

void recordWorldEnvironment(
    WorldDiagnosticStage stage,
    Level*               level,
    Dimension*           dimension,
    float                partialTick,
    uint64_t             tickToken = 0
) noexcept;

void     recordRenderDiagnostics(RenderDiagnosticStage stage, ClientInstance* client = nullptr) noexcept;
uint64_t recordReplayAdmissionBoundary(std::string_view phase, uint64_t span = 0) noexcept;
void     recordReplayPacksInfo(
    std::string_view               source,
    ResourcePacksInfoPacket const& packet,
    bool                           selected,
    uint64_t                       span
) noexcept;
void recordReplayPackStack(
    std::string_view               source,
    ResourcePackStackPacket const& packet,
    bool                           selected,
    uint64_t                       span
) noexcept;
void recordReplayWorldSettings(std::string_view source, LevelSettings const& settings, uint64_t span = 0) noexcept;
void recordReplayRequestedGraphicsMode(MinecraftScreenModel const& model) noexcept;

} // namespace playback::exporting
