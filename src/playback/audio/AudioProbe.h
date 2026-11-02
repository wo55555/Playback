#pragma once

#include <functional>

class ClientInstance;

namespace playback::audio::fmod {
struct CApi;
}

// Phase 0 diagnostics for audio export (P0-1/2/3/4/6/9). Probe branch only.
namespace playback::audio::probe {

bool install();
void uninstall();

// Main thread, once per ClientInstance::update.
void onClientUpdate(ClientInstance& client);

// Policy hooks the export-capture module registers so it can drive the game's FMOD system without re-hooking.
// All default to no-op (pure probe behavior). Set by playback::audio::exportcap after install().
struct Policy {
    // Return true to swallow a System::update call (gate it) instead of calling the original.
    std::function<bool()> shouldGateUpdate;
    // Called by the init hook after the original init returns success; the export module caches the system.
    std::function<void(void* system)> onSystemReady;
    // Called by the setOutput hook before the original runs; may rewrite the output type (e.g. to NOSOUND_NRT).
    std::function<void(int& outputType)> overrideOutputType;
    // SoundSystemFMOD::enable (slot 6, mixerSuspend/Resume). Return true to swallow the call; the export replays it.
    std::function<bool(bool enable)> deferEnable;
    // SoundSystemFMOD::setOutputDevice (slot 4, setDriver + setOutput). Return true to swallow; deviceId is 16 bytes.
    std::function<bool(void const* deviceId)> deferSetOutputDevice;
    // Ore UI stream pause helper (0x6E8B410) busy-waits on Channel::getPosition, which never advances without a mixer
    // thread. Return true to skip it.
    std::function<bool()> skipStreamPauseWait;
};

void setPolicy(Policy policy);

// Accessors for the export-capture module. origUpdate drives the gated FMOD mixer; currentSystem is the live system.
using OrigUpdateFn = int (*)(void* system);
OrigUpdateFn origUpdateFn();
void*        currentSystem();

// SoundSystemFMOD::update(float) hook (vtable slot 53). The game layer's per-frame audio tick: it unpause channels,
// submits 3D listeners, and cleans up dead sounds on top of the bare FMOD System::update. The export must drive this
// (not the bare update) so channels actually play.
using OrigUpdateFloatFn = void (*)(void* soundSystem, float dt);
OrigUpdateFloatFn origUpdateFloatFn();
void*             currentSoundSystem(); // live SoundSystemFMOD instance captured by the update(float) hook

// Gate for the update(float) hook: when the export owns the mixer, the game's own calls are swallowed; manual drives
// set a thread_local bypass so the same detour runs the origin.
bool shouldGateUpdateFloat();
void setGateUpdateFloat(bool gate);
// RAII bypass so a manual drive through the detour runs the origin instead of being swallowed.
struct UpdateFloatDriveScope {
    UpdateFloatDriveScope();
    ~UpdateFloatDriveScope();
};

// Originals of the game-layer calls the export defers while it owns the FMOD output (null if the hook is missing).
using OrigEnableFn          = void (*)(void* soundSystem, bool enable);
using OrigSetOutputDeviceFn = void (*)(void* soundSystem, void const* deviceId);
OrigEnableFn          origEnableFn();
OrigSetOutputDeviceFn origSetOutputDeviceFn();
// True when enable, setOutputDevice and the stream-pause wait are all hooked; the NRT export needs all three.
bool outputGuardsInstalled();

// Internal surface for playback::audio::exportcap only. Not for general use.
namespace exportcap_internal {
playback::audio::fmod::CApi const& api();
}

} // namespace playback::audio::probe
