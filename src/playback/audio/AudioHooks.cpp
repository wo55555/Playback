#include "AudioHooks.h"

#include "FmodMinimal.h"
#include "PreviewAudio.h"

#include "playback/Playback.h"

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

namespace playback::audio::hooks {

namespace {

// FMOD 2.02.30 (fmod.dll in 1.26.20.04); the C handle and the C++ object are the same pointer.
using InitFn = int (*)(void* system, int maxChannels, unsigned flags, void* extra);
using PlaySoundFn = int (*)(void* system, void* sound, void* group, bool paused, void** channel);

// SoundSystemFMOD on 1.26.20.04.
constexpr uintptr_t SoundSystemFmodVtableRva = 0xAEE7FE0;
constexpr size_t    UpdateFloatVtableSlot    = 53;
constexpr uintptr_t UpdateFloatRva           = 0x49AD650;
// Calls that would fight the export for the FMOD output while it runs NOSOUND_NRT.
constexpr size_t    SetOutputDeviceVtableSlot = 4; // setDriver(guid match) + setOutput(AUTODETECT)
constexpr uintptr_t SetOutputDeviceRva        = 0x49A5DA0;
constexpr size_t    EnableVtableSlot          = 6; // mixerSuspend / mixerResume
constexpr uintptr_t EnableRva                 = 0x49A6B10;
// Ore UI stream pause: System::update once, then spins on Channel::getPosition. Not virtual; reached only through the
// SoundEngine thunk below (add rcx,18h; jmp rel32), which doubles as the build check.
constexpr uintptr_t StreamPauseRva      = 0x49AF080;
constexpr uintptr_t StreamPauseThunkRva = 0x606600;
using StreamPauseFn                     = uint64_t (*)(void* soundSystem, uint64_t key);

struct Hook {
    void*               target{};
    ll::memory::FuncPtr original{};
    bool                installed{};
};

Hook gUpdateHook;
Hook gInitHook;
Hook gUpdateFloatHook;
Hook gEnableHook;
Hook gSetOutputDeviceHook;
Hook gStreamPauseHook;
Hook gPlaySoundHook;

fmod::CApi         gApi;
Policy             gPolicy;
std::atomic<void*> gSystem{nullptr};
std::atomic<void*> gSoundSystem{nullptr};
std::atomic<bool>  gGateUpdateFloat{false};
thread_local bool  gDrivingUpdateFloat = false;

ll::io::Logger& logger() { return Playback::getInstance().getSelf().getLogger(); }

template <class Fn>
void resolve(HMODULE module, char const* name, Fn& out) {
    out = reinterpret_cast<Fn>(GetProcAddress(module, name));
}

int updateDetour(void* system) {
    if (gPolicy.shouldGateUpdate && gPolicy.shouldGateUpdate()) return 0; // FMOD_OK; the export ledger drives mixing.
    gSystem.store(system);
    return reinterpret_cast<OrigUpdateFn>(gUpdateHook.original)(system);
}

int initDetour(void* system, int maxChannels, unsigned flags, void* extra) {
    int const result = reinterpret_cast<InitFn>(gInitHook.original)(system, maxChannels, flags, extra);
    if (result == 0 && gPolicy.onSystemReady) gPolicy.onSystemReady(system);
    return result;
}

int playSoundDetour(void* system, void* sound, void* group, bool paused, void** channel) {
    int const result = reinterpret_cast<PlaySoundFn>(gPlaySoundHook.original)(system, sound, group, paused, channel);
    if (result == 0 && channel && *channel && group && preview::shouldDropNewSounds() && gApi.channelStop
        && gApi.groupGetName) {
        // Stop only world sounds; music and accessibility speech have separate groups.
        char name[128]{};
        if (gApi.groupGetName(group, name, sizeof(name)) == 0 && std::strncmp(name, "Sound", 5) == 0)
            gApi.channelStop(*channel);
    }
    return result;
}

// Capture the live instance, then either swallow the game's own drive (while the export owns the mixer) or run the
// game-layer tick (unpause + listeners + cleanup + one System::update).
void updateFloatDetour(void* soundSystem, float dt) {
    gSoundSystem.store(soundSystem, std::memory_order_relaxed);
    if (gGateUpdateFloat.load(std::memory_order_relaxed) && !gDrivingUpdateFloat) return;
    reinterpret_cast<OrigUpdateFloatFn>(gUpdateFloatHook.original)(soundSystem, dt);
}

void enableDetour(void* soundSystem, bool enable) {
    if (gPolicy.deferEnable && gPolicy.deferEnable(enable)) return;
    reinterpret_cast<OrigEnableFn>(gEnableHook.original)(soundSystem, enable);
}

void setOutputDeviceDetour(void* soundSystem, void const* deviceId) {
    if (gPolicy.deferSetOutputDevice && gPolicy.deferSetOutputDevice(deviceId)) return;
    reinterpret_cast<OrigSetOutputDeviceFn>(gSetOutputDeviceHook.original)(soundSystem, deviceId);
}

uint64_t streamPauseDetour(void* soundSystem, uint64_t key) {
    if (gPolicy.skipStreamPauseWait && gPolicy.skipStreamPauseWait()) return 0;
    return reinterpret_cast<StreamPauseFn>(gStreamPauseHook.original)(soundSystem, key);
}

template <class Detour>
bool installHook(Hook& hook, Detour detour, char const* name) {
    if (hook.installed) return true;
    if (!hook.target) {
        logger().warn("Export audio hooks: {} not found", name);
        return false;
    }
    if (ll::memory::hook(hook.target, ll::memory::toFuncPtr(detour), &hook.original, ll::memory::HookPriority::Normal)
        != 0) {
        logger().warn("Export audio hooks: unable to hook {}", name);
        return false;
    }
    hook.installed = true;
    return true;
}

template <class Detour>
void removeHook(Hook& hook, Detour detour) {
    if (hook.installed && ll::memory::unhook(hook.target, ll::memory::toFuncPtr(detour))) hook.installed = false;
}

uintptr_t gameImageSize(uintptr_t base) {
    auto const dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    auto const nt  = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

// The RVAs belong to one game build; the vtable slot must point at the function, otherwise this is another build.
void* resolveSoundSystemSlot(size_t slotIndex, uintptr_t fnRva, char const* name) {
    auto const      base    = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto const      size    = gameImageSize(base);
    uintptr_t const slotRva = SoundSystemFmodVtableRva + slotIndex * sizeof(void*);
    if (slotRva + sizeof(void*) > size || fnRva >= size) {
        logger().warn("Export audio hooks: {} RVA is outside this game image", name);
        return nullptr;
    }
    uintptr_t const slot = *reinterpret_cast<uintptr_t const*>(base + slotRva);
    if (slot != base + fnRva) {
        logger().warn(
            "Export audio hooks: {} vtable slot mismatch (slot rva=0x{:X}); unsupported game build",
            name,
            slot - base
        );
        return nullptr;
    }
    return reinterpret_cast<void*>(slot);
}

// The stream pause thunk is checked before hooking so an unexpected game build is rejected.
void* resolveStreamPause() {
    auto const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto const size = gameImageSize(base);
    if (StreamPauseThunkRva + 9 > size || StreamPauseRva >= size) return nullptr;
    auto const*                      thunk = reinterpret_cast<uint8_t const*>(base + StreamPauseThunkRva);
    constexpr std::array<uint8_t, 5> Expected{0x48, 0x83, 0xC1, 0x18, 0xE9};
    if (std::memcmp(thunk, Expected.data(), Expected.size()) != 0) {
        logger().warn("Export audio hooks: stream pause thunk bytes differ; unsupported game build");
        return nullptr;
    }
    int32_t rel = 0;
    std::memcpy(&rel, thunk + 5, sizeof(rel));
    uintptr_t const target = base + StreamPauseThunkRva + 9 + intptr_t(rel);
    if (target != base + StreamPauseRva) {
        logger().warn("Export audio hooks: stream pause thunk target mismatch (rva=0x{:X})", target - base);
        return nullptr;
    }
    return reinterpret_cast<void*>(target);
}

} // namespace

bool install() {
    HMODULE const fmod = GetModuleHandleW(L"fmod.dll");
    if (!fmod) {
        logger().warn("Export audio hooks: fmod.dll is not loaded; audio export disabled");
        return false;
    }
    resolve(fmod, "FMOD_System_GetOutput", gApi.getOutput);
    resolve(fmod, "FMOD_System_SetOutput", gApi.setOutput);
    resolve(fmod, "FMOD_System_GetDriver", gApi.getDriver);
    resolve(fmod, "FMOD_System_SetDriver", gApi.setDriver);
    resolve(fmod, "FMOD_System_GetChannelsPlaying", gApi.getChannelsPlaying);
    resolve(fmod, "FMOD_System_GetMasterChannelGroup", gApi.getMasterChannelGroup);
    resolve(fmod, "FMOD_System_CreateDSP", gApi.createDSP);
    resolve(fmod, "FMOD_ChannelGroup_GetNumDSPs", gApi.groupGetNumDSPs);
    resolve(fmod, "FMOD_ChannelGroup_GetDSPIndex", gApi.groupGetDSPIndex);
    resolve(fmod, "FMOD_ChannelGroup_AddDSP", gApi.groupAddDSP);
    resolve(fmod, "FMOD_ChannelGroup_RemoveDSP", gApi.groupRemoveDSP);
    resolve(fmod, "FMOD_ChannelGroup_Stop", gApi.groupStop);
    resolve(fmod, "FMOD_DSP_Release", gApi.dspRelease);
    resolve(fmod, "FMOD_Channel_Stop", gApi.channelStop);
    resolve(fmod, "FMOD_ChannelGroup_GetName", gApi.groupGetName);

    gUpdateHook.target = reinterpret_cast<void*>(GetProcAddress(fmod, "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ"));
    gInitHook.target =
        reinterpret_cast<void*>(GetProcAddress(fmod, "?init@System@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAX@Z"));
    gUpdateFloatHook.target = resolveSoundSystemSlot(UpdateFloatVtableSlot, UpdateFloatRva, "update(float)");
    gEnableHook.target      = resolveSoundSystemSlot(EnableVtableSlot, EnableRva, "enable");
    gSetOutputDeviceHook.target =
        resolveSoundSystemSlot(SetOutputDeviceVtableSlot, SetOutputDeviceRva, "setOutputDevice");
    gStreamPauseHook.target = resolveStreamPause();
    gPlaySoundHook.target   = reinterpret_cast<void*>(GetProcAddress(
        fmod,
        "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z"
    ));
    if (!installHook(gPlaySoundHook, &playSoundDetour, "System::playSound") || !gApi.channelStop || !gApi.groupGetName)
        logger().warn("Preview audio: silent while paused is unavailable");

    std::array const installed{
        installHook(gUpdateHook, &updateDetour, "System::update"),
        installHook(gInitHook, &initDetour, "System::init"),
        installHook(gUpdateFloatHook, &updateFloatDetour, "update(float)"),
        installHook(gEnableHook, &enableDetour, "enable"),
        installHook(gSetOutputDeviceHook, &setOutputDeviceDetour, "setOutputDevice"),
        installHook(gStreamPauseHook, &streamPauseDetour, "stream pause"),
    };
    bool const complete = std::ranges::all_of(installed, std::identity{});
    if (!complete) logger().warn("Export audio hooks: incomplete; audio export disabled");
    return complete;
}

void uninstall() {
    preview::reset();
    gPolicy = {};
    gGateUpdateFloat.store(false, std::memory_order_relaxed);
    removeHook(gPlaySoundHook, &playSoundDetour);
    removeHook(gStreamPauseHook, &streamPauseDetour);
    removeHook(gSetOutputDeviceHook, &setOutputDeviceDetour);
    removeHook(gEnableHook, &enableDetour);
    removeHook(gUpdateFloatHook, &updateFloatDetour);
    removeHook(gInitHook, &initDetour);
    removeHook(gUpdateHook, &updateDetour);
}

void setPolicy(Policy policy) { gPolicy = std::move(policy); }

OrigUpdateFn origUpdateFn() { return reinterpret_cast<OrigUpdateFn>(gUpdateHook.original); }

void* currentSystem() { return gSystem.load(); }

OrigUpdateFloatFn origUpdateFloatFn() { return reinterpret_cast<OrigUpdateFloatFn>(gUpdateFloatHook.original); }

void* currentSoundSystem() { return gSoundSystem.load(); }

void setGateUpdateFloat(bool gate) { gGateUpdateFloat.store(gate, std::memory_order_relaxed); }

UpdateFloatDriveScope::UpdateFloatDriveScope() { gDrivingUpdateFloat = true; }

UpdateFloatDriveScope::~UpdateFloatDriveScope() { gDrivingUpdateFloat = false; }

OrigEnableFn origEnableFn() {
    return gEnableHook.installed ? reinterpret_cast<OrigEnableFn>(gEnableHook.original) : nullptr;
}

OrigSetOutputDeviceFn origSetOutputDeviceFn() {
    return gSetOutputDeviceHook.installed ? reinterpret_cast<OrigSetOutputDeviceFn>(gSetOutputDeviceHook.original)
                                          : nullptr;
}

fmod::CApi const& fmodApi() { return gApi; }

} // namespace playback::audio::hooks
