#pragma once

#include <functional>

namespace playback::audio::fmod {
struct CApi;
}

// Hooks on the game's FMOD system that let the export-audio capture drive the mixer itself.
namespace playback::audio::hooks {

// Returns true only when every hook the audio export needs is installed.
bool install();
void uninstall();

// Callbacks the export-capture module registers so it can drive the game's FMOD system. All default to no-op.
struct Policy {
    // Return true to swallow a System::update call (gate it) instead of calling the original.
    std::function<bool()> shouldGateUpdate;
    // Called after System::init succeeds; a new system during an export means the export lost its output.
    std::function<void(void* system)> onSystemReady;
    // SoundSystemFMOD::enable (slot 6, mixerSuspend/Resume). Return true to swallow the call; the export replays it.
    std::function<bool(bool enable)> deferEnable;
    // SoundSystemFMOD::setOutputDevice (slot 4, setDriver + setOutput). Return true to swallow; deviceId is 16 bytes.
    std::function<bool(void const* deviceId)> deferSetOutputDevice;
    // Ore UI stream pause helper busy-waits on Channel::getPosition, which never advances without a mixer thread.
    // Return true to skip it.
    std::function<bool()> skipStreamPauseWait;
};

void setPolicy(Policy policy);

// origUpdate drives the gated FMOD mixer; currentSystem is the live system.
using OrigUpdateFn = int (*)(void* system);
OrigUpdateFn origUpdateFn();
void*        currentSystem();

// SoundSystemFMOD::update(float), the game layer's per-frame audio tick: it unpauses channels, submits 3D listeners,
// and cleans up dead sounds on top of the bare FMOD System::update. The export must drive this (not the bare update)
// so channels actually play.
using OrigUpdateFloatFn = void (*)(void* soundSystem, float dt);
OrigUpdateFloatFn origUpdateFloatFn();
void*             currentSoundSystem(); // live SoundSystemFMOD instance captured by the update(float) hook

// While the export owns the mixer, the game's own update(float) calls are swallowed.
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

// fmod.dll C API entry points resolved by install().
fmod::CApi const& fmodApi();

} // namespace playback::audio::hooks
