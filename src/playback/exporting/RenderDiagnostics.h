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
