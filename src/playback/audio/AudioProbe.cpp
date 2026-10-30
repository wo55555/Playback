#include "AudioProbe.h"

#include "FmodMinimal.h"

#include "playback/Playback.h"
#include "playback/exporting/ExportActivity.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numbers>
#include <string>

namespace playback::audio::probe {

namespace {

// FMOD 2.02.30 (fmod.dll in 1.26.20.04); the C handle and the C++ object are the same pointer.
struct FmodVector {
    float x, y, z;
};

using UpdateFn      = int (*)(void* system);
using InitFn        = int (*)(void* system, int maxChannels, unsigned flags, void* extra);
using SetOutputFn   = int (*)(void* system, int output);
using PlaySoundFn   = int (*)(void* system, void* sound, void* group, bool paused, void** channel);
using SetListenerFn = int (*)(
    void*             system,
    int               listener,
    FmodVector const* pos,
    FmodVector const* vel,
    FmodVector const* forward,
    FmodVector const* up
);

using fmod::CApi;

// Registered by playback::audio::exportcap; defaults are no-ops (pure probe behavior).
playback::audio::probe::Policy gPolicy;

enum Bucket : uint8_t { Normal, ReplayPlaying, ReplayPaused, ReplaySeeking, ExportRunning, ExportSeeking, BucketCount };

constexpr std::array<char const*, BucketCount> BucketNames{
    "normal",
    "replayPlaying",
    "replayPaused",
    "replaySeeking",
    "export",
    "exportSeeking",
};

constexpr int PlayDetailCapPerBucket = 40;
constexpr int ListenerLineCap        = 900;
constexpr int TopologyLineCap        = 400;

struct Hooks {
    void* update{};
    void* init{};
    void* setOutput{};
    void* playSound{};
    void* setListener{};
    bool  updateInstalled{};
    bool  initInstalled{};
    bool  setOutputInstalled{};
    bool  playSoundInstalled{};
    bool  setListenerInstalled{};
};

Hooks               gHooks;
CApi                gApi;
ll::memory::FuncPtr gOriginalUpdate{};
ll::memory::FuncPtr gOriginalInit{};
ll::memory::FuncPtr gOriginalSetOutput{};
ll::memory::FuncPtr gOriginalPlaySound{};
ll::memory::FuncPtr gOriginalSetListener{};
thread_local bool   gInProbe = false;

std::atomic<uint8_t>  gBucket{Normal};
std::atomic<uint32_t> gMainThreadId{0};
std::atomic<void*>    gSystem{nullptr};
std::atomic<uint32_t> gDistinctSystems{0};

// P0-1 update statistics.
std::atomic<uint64_t> gUpdateCalls{0};
std::atomic<uint32_t> gUpdateThreadId{0};
std::atomic<uint32_t> gUpdateThreadChanges{0};
std::atomic<uint64_t> gUpdateOnMainThread{0};
std::atomic<uint64_t> gUpdateFailures{0};
std::atomic<int>      gLastUpdateResult{0};

// P0-9.
std::atomic<uint32_t> gInitCalls{0};
std::atomic<uint32_t> gSetOutputCalls{0};

// P0-3 / P0-6.
std::array<std::atomic<uint64_t>, BucketCount> gPlayCalls{};
std::array<std::atomic<uint64_t>, BucketCount> gPlayFailures{};
std::array<std::atomic<int>, BucketCount>      gPlayDetailLines{};
std::atomic<uint32_t>                          gPlayThreadId{0};

// P0-4.
struct ListenerSample {
    FmodVector                            pos{}, vel{}, forward{}, up{};
    int                                   listener{};
    uint32_t                              threadId{};
    uint64_t                              calls{};
    std::chrono::steady_clock::time_point at{};
};
std::mutex     gListenerMutex;
ListenerSample gListener;
int            gListenerLines = 0;

int gTopologyLines = 0;

// SoundSystemFMOD::update(float) on 1.26.20.04. Gate the game-layer audio tick.
constexpr uintptr_t UpdateFloatRva          = 0x49AD650;
constexpr uintptr_t SoundSystemFmodVtableRva = 0xAEE7FE0;
constexpr size_t    UpdateFloatVtableSlot   = 53;
void*               gUpdateFloatTarget = nullptr;
ll::memory::FuncPtr gOriginalUpdateFloat{};
bool                gUpdateFloatInstalled{};
std::atomic<void*>  gSoundSystem{nullptr};
std::atomic<bool>   gGateUpdateFloat{false};
thread_local bool   gDrivingUpdateFloat = false;

// Calls that would fight the export for the FMOD output while it runs NOSOUND_NRT (1.26.20.04 RVAs, same vtable).
constexpr size_t    SetOutputDeviceVtableSlot = 4; // setDriver(guid match) + setOutput(AUTODETECT)
constexpr uintptr_t SetOutputDeviceRva        = 0x49A5DA0;
constexpr size_t    EnableVtableSlot          = 6; // mixerSuspend / mixerResume
constexpr uintptr_t EnableRva                 = 0x49A6B10;
// Ore UI stream pause: System::update once, then spins on Channel::getPosition. Not virtual; reached only through the
// SoundEngine thunk below (add rcx,18h; jmp rel32), which doubles as the build check.
constexpr uintptr_t StreamPauseRva      = 0x49AF080;
constexpr uintptr_t StreamPauseThunkRva = 0x606600;
using EnableFn          = void (*)(void* soundSystem, bool enable);
using SetOutputDeviceFn = void (*)(void* soundSystem, void const* deviceId);
using StreamPauseFn     = uint64_t (*)(void* soundSystem, uint64_t key);
struct GuardHook {
    void*               target{};
    ll::memory::FuncPtr original{};
    bool                installed{};
};
GuardHook gEnableHook;
GuardHook gSetOutputDeviceHook;
GuardHook gStreamPauseHook;

ll::io::Logger& logger() { return Playback::getInstance().getSelf().getLogger(); }

int64_t msSince(std::chrono::steady_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - at).count();
}

template <class Fn>
bool resolve(HMODULE module, char const* name, Fn& out) {
    out = reinterpret_cast<Fn>(GetProcAddress(module, name));
    return out != nullptr;
}

std::string groupName(void* group) {
    char name[128]{};
    if (!gApi.groupGetName || gApi.groupGetName(group, name, sizeof(name)) != 0) return "?";
    return name;
}

// Volume/mute rounded so slider moves show up as a changed signature.
std::string describeTopology(void* group, int depth, int& budget) {
    if (!group || depth > 6 || budget <= 0) return {};
    --budget;
    float volume   = -1.0f;
    int   mute     = -1;
    int   paused   = -1;
    int   channels = -1;
    int   dsps     = -1;
    int   children = 0;
    if (gApi.groupGetVolume) gApi.groupGetVolume(group, &volume);
    if (gApi.groupGetMute) gApi.groupGetMute(group, &mute);
    if (gApi.groupGetPaused) gApi.groupGetPaused(group, &paused);
    if (gApi.groupGetNumChannels) gApi.groupGetNumChannels(group, &channels);
    if (gApi.groupGetNumDSPs) gApi.groupGetNumDSPs(group, &dsps);
    if (gApi.groupGetNumGroups) gApi.groupGetNumGroups(group, &children);

    char line[256];
    std::snprintf(
        line,
        sizeof(line),
        "%*s[%s vol=%.3f mute=%d paused=%d ch=%d dsps=%d sub=%d]",
        depth * 2,
        "",
        groupName(group).c_str(),
        volume,
        mute,
        paused,
        channels,
        dsps,
        children
    );
    std::string out = line;
    for (int i = 0; i < children && gApi.groupGetGroup; ++i) {
        void* child{};
        if (gApi.groupGetGroup(group, i, &child) == 0 && child) out += describeTopology(child, depth + 1, budget);
    }
    return out;
}

void logSystemInfo(void* system, char const* reason) {
    unsigned  version    = 0;
    int       output     = -1;
    int       driver     = -1;
    int       sampleRate = 0;
    int       speakers   = 0;
    int       rawSpk     = 0;
    unsigned  bufferLen  = 0;
    int       numBuffers = 0;
    int       playing    = -1;
    int       real       = -1;
    float     doppler = 0, distance = 0, rolloff = 0;
    int       listeners = -1;
    int const rv        = gApi.getVersion ? gApi.getVersion(system, &version) : -1;
    if (gApi.getOutput) gApi.getOutput(system, &output);
    if (gApi.getDriver) gApi.getDriver(system, &driver);
    int const rf = gApi.getSoftwareFormat ? gApi.getSoftwareFormat(system, &sampleRate, &speakers, &rawSpk) : -1;
    int const rb = gApi.getDSPBufferSize ? gApi.getDSPBufferSize(system, &bufferLen, &numBuffers) : -1;
    if (gApi.getChannelsPlaying) gApi.getChannelsPlaying(system, &playing, &real);
    if (gApi.get3DSettings) gApi.get3DSettings(system, &doppler, &distance, &rolloff);
    if (gApi.get3DNumListeners) gApi.get3DNumListeners(system, &listeners);
    logger().info(
        "Audio probe system ({}): system={} version=0x{:08x}(r{}) output={} driver={} rate={} speakerMode={} "
        "rawSpeakers={}(r{}) dspBuffer={}x{}(r{}) playing={}/{} 3d=[doppler={} distance={} rolloff={}] "
        "listeners={} updateThread={} mainThread={} fmodstudio={} initSeen={} setOutputSeen={}",
        reason,
        system,
        version,
        rv,
        output,
        driver,
        sampleRate,
        speakers,
        rawSpk,
        rf,
        bufferLen,
        numBuffers,
        rb,
        playing,
        real,
        doppler,
        distance,
        rolloff,
        listeners,
        gUpdateThreadId.load(),
        gMainThreadId.load(),
        GetModuleHandleW(L"fmodstudio.dll") != nullptr,
        gInitCalls.load(),
        gSetOutputCalls.load()
    );
}

void sampleTopology(void* system, bool force) {
    static std::string                           lastSignature;
    static std::chrono::steady_clock::time_point lastAt{};
    if (!force && msSince(lastAt) < 2000) return;
    lastAt = std::chrono::steady_clock::now();
    void* master{};
    if (!gApi.getMasterChannelGroup || gApi.getMasterChannelGroup(system, &master) != 0 || !master) return;
    int         budget    = 64;
    std::string signature = describeTopology(master, 0, budget);
    if (!force && signature == lastSignature) return;
    lastSignature = signature;
    if (gTopologyLines >= TopologyLineCap) {
        if (gTopologyLines++ == TopologyLineCap) logger().info("Audio probe topology: cap reached");
        return;
    }
    ++gTopologyLines;
    logger().info(
        "Audio probe topology bucket={} groupsVisited={} {}",
        BucketNames[gBucket.load()],
        64 - budget,
        signature
    );
}

int updateDetour(void* system) {
    if (gInProbe) return reinterpret_cast<UpdateFn>(gOriginalUpdate)(system);
    if (gPolicy.shouldGateUpdate && gPolicy.shouldGateUpdate()) return 0; // FMOD_OK; the export ledger drives mixing.
    gInProbe = true;

    auto const calls    = gUpdateCalls.fetch_add(1) + 1;
    auto const threadId = static_cast<uint32_t>(GetCurrentThreadId());
    auto const previous = gUpdateThreadId.exchange(threadId);
    if (previous != 0 && previous != threadId) gUpdateThreadChanges.fetch_add(1);
    if (threadId == gMainThreadId.load(std::memory_order_relaxed)) gUpdateOnMainThread.fetch_add(1);

    void* const known = gSystem.load();
    if (known != system) {
        gSystem.store(system);
        gDistinctSystems.fetch_add(1);
        logSystemInfo(system, known ? "system changed" : "first update");
        sampleTopology(system, true);
    } else {
        static std::chrono::steady_clock::time_point lastInfo = std::chrono::steady_clock::now();
        if (msSince(lastInfo) >= 30000) {
            lastInfo = std::chrono::steady_clock::now();
            logSystemInfo(system, "periodic");
        }
        sampleTopology(system, false);
    }
    (void)calls;

    gInProbe         = false;
    int const result = reinterpret_cast<UpdateFn>(gOriginalUpdate)(system);
    gLastUpdateResult.store(result, std::memory_order_relaxed);
    if (result != 0) gUpdateFailures.fetch_add(1);
    return result;
}

// SoundSystemFMOD::update(float) detour. Capture the live instance, then either swallow the game's own drive (while the
// export owns the mixer) or run the game-layer tick (unpause + listeners + cleanup + one System::update).
void updateFloatDetour(void* soundSystem, float dt) {
    gSoundSystem.store(soundSystem, std::memory_order_relaxed);
    if (gGateUpdateFloat.load(std::memory_order_relaxed) && !gDrivingUpdateFloat) return;
    reinterpret_cast<OrigUpdateFloatFn>(gOriginalUpdateFloat)(soundSystem, dt);
}

void enableDetour(void* soundSystem, bool enable) {
    if (gPolicy.deferEnable && gPolicy.deferEnable(enable)) {
        logger().info("Audio probe SoundSystemFMOD::enable({}) deferred during export", enable);
        return;
    }
    reinterpret_cast<EnableFn>(gEnableHook.original)(soundSystem, enable);
}

void setOutputDeviceDetour(void* soundSystem, void const* deviceId) {
    if (gPolicy.deferSetOutputDevice && gPolicy.deferSetOutputDevice(deviceId)) {
        logger().info("Audio probe SoundSystemFMOD::setOutputDevice deferred during export");
        return;
    }
    reinterpret_cast<SetOutputDeviceFn>(gSetOutputDeviceHook.original)(soundSystem, deviceId);
}

uint64_t streamPauseDetour(void* soundSystem, uint64_t key) {
    if (gPolicy.skipStreamPauseWait && gPolicy.skipStreamPauseWait()) {
        logger().info("Audio probe stream pause skipped during export (key=0x{:x})", key);
        return 0;
    }
    return reinterpret_cast<StreamPauseFn>(gStreamPauseHook.original)(soundSystem, key);
}

int initDetour(void* system, int maxChannels, unsigned flags, void* extra) {
    int const result = reinterpret_cast<InitFn>(gOriginalInit)(system, maxChannels, flags, extra);
    gInitCalls.fetch_add(1);
    logger().info(
        "Audio probe System::init system={} maxChannels={} flags=0x{:x} extra={} result={} thread={}",
        system,
        maxChannels,
        flags,
        extra,
        result,
        GetCurrentThreadId()
    );
    if (result == 0 && gPolicy.onSystemReady) gPolicy.onSystemReady(system);
    return result;
}

int setOutputDetour(void* system, int output) {
    if (gPolicy.overrideOutputType) gPolicy.overrideOutputType(output);
    int const result = reinterpret_cast<SetOutputFn>(gOriginalSetOutput)(system, output);
    gSetOutputCalls.fetch_add(1);
    logger().info(
        "Audio probe System::setOutput system={} output={} result={} thread={}",
        system,
        output,
        result,
        GetCurrentThreadId()
    );
    return result;
}

int playSoundDetour(void* system, void* sound, void* group, bool paused, void** channel) {
    int const result = reinterpret_cast<PlaySoundFn>(gOriginalPlaySound)(system, sound, group, paused, channel);
    if (gInProbe) return result;
    gInProbe          = true;
    auto const bucket = gBucket.load(std::memory_order_relaxed);
    gPlayCalls[bucket].fetch_add(1);
    if (result != 0) gPlayFailures[bucket].fetch_add(1);
    gPlayThreadId.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);
    int const line = gPlayDetailLines[bucket].fetch_add(1);
    if (line < PlayDetailCapPerBucket) {
        char name[160]{};
        if (!sound || !gApi.soundGetName || gApi.soundGetName(sound, name, sizeof(name)) != 0) name[0] = '?';
        logger().info(
            "Audio probe playSound bucket={} n={} sound={} group={} paused={} result={} thread={}",
            BucketNames[bucket],
            line + 1,
            name,
            group ? groupName(group) : std::string("<master>"),
            paused,
            result,
            GetCurrentThreadId()
        );
    } else if (line == PlayDetailCapPerBucket) {
        logger().info("Audio probe playSound bucket={}: detail cap reached", BucketNames[bucket]);
    }
    gInProbe = false;
    return result;
}

int setListenerDetour(
    void*             system,
    int               listener,
    FmodVector const* pos,
    FmodVector const* vel,
    FmodVector const* forward,
    FmodVector const* up
) {
    {
        std::lock_guard lock(gListenerMutex);
        if (pos) gListener.pos = *pos;
        if (vel) gListener.vel = *vel;
        if (forward) gListener.forward = *forward;
        if (up) gListener.up = *up;
        gListener.listener = listener;
        gListener.threadId = static_cast<uint32_t>(GetCurrentThreadId());
        gListener.at       = std::chrono::steady_clock::now();
        ++gListener.calls;
    }
    return reinterpret_cast<SetListenerFn>(gOriginalSetListener)(system, listener, pos, vel, forward, up);
}

Bucket computeBucket() {
    auto&      replay    = replay::ReplaySession::getInstance();
    bool const exporting = exporting::isExportActivityActive();
    bool const seeking   = replay.isActive() && replay.isSeekPending();
    if (exporting) return seeking ? ExportSeeking : ExportRunning;
    if (!replay.isActive()) return Normal;
    if (seeking) return ReplaySeeking;
    return replay.isPaused() ? ReplayPaused : ReplayPlaying;
}

void logPlaySummary(Bucket bucket) {
    static std::chrono::steady_clock::time_point lastAt = std::chrono::steady_clock::now();
    static std::array<uint64_t, BucketCount>     lastCalls{};
    static uint64_t                              lastUpdates = 0;
    if (msSince(lastAt) < 5000) return;
    auto const elapsedMs = msSince(lastAt);
    lastAt               = std::chrono::steady_clock::now();

    std::array<uint64_t, BucketCount> calls{};
    bool                              changed = false;
    for (size_t i = 0; i < BucketCount; ++i) {
        calls[i]  = gPlayCalls[i].load();
        changed  |= calls[i] != lastCalls[i];
    }
    auto const updates     = gUpdateCalls.load();
    auto const updateDelta = updates - lastUpdates;
    lastUpdates            = updates;
    std::string perBucket;
    for (size_t i = 0; i < BucketCount; ++i) {
        perBucket += std::string(BucketNames[i]) + "=" + std::to_string(calls[i]) + "(+"
                   + std::to_string(calls[i] - lastCalls[i]) + ",fail " + std::to_string(gPlayFailures[i].load())
                   + ") ";
    }
    lastCalls = calls;
    // Without new plays, the summary only repeats every 30 s so idle menus stay quiet.
    static int quietWindows = 0;
    if (!changed && ++quietWindows < 6) return;
    quietWindows = 0;
    logger().info(
        "Audio probe summary bucket={} updates={}(+{} in {}ms, {:.1f}/s) updateThread={} threadChanges={} "
        "onMain={} updateFail={} lastResult={} playThread={} plays: {}",
        BucketNames[bucket],
        updates,
        updateDelta,
        elapsedMs,
        elapsedMs > 0 ? updateDelta * 1000.0 / elapsedMs : 0.0,
        gUpdateThreadId.load(),
        gUpdateThreadChanges.load(),
        gUpdateOnMainThread.load(),
        gUpdateFailures.load(),
        gLastUpdateResult.load(),
        gPlayThreadId.load(),
        perBucket
    );
}

void logListener(ClientInstance& client, Bucket bucket) {
    static std::chrono::steady_clock::time_point lastAt{};
    auto const                                   intervalMs = bucket == Normal ? 10000 : 1000;
    if (msSince(lastAt) < intervalMs) return;
    lastAt = std::chrono::steady_clock::now();
    if (gListenerLines >= ListenerLineCap) {
        if (gListenerLines++ == ListenerLineCap) logger().info("Audio probe listener: cap reached");
        return;
    }

    ListenerSample sample;
    {
        std::lock_guard lock(gListenerMutex);
        sample = gListener;
    }
    if (sample.calls == 0) {
        ++gListenerLines;
        logger().info("Audio probe listener bucket={}: set3DListenerAttributes not seen yet", BucketNames[bucket]);
        return;
    }

    auto* player = client.getLocalPlayer();
    if (!player) return;
    ::Vec3 const position = player->getPosition();
    ::Vec2 const rotation = player->getRotation();
    // Bedrock rotation is (pitch, yaw) in degrees; forward for yaw 0 is +Z.
    float const      pitch = rotation.x * std::numbers::pi_v<float> / 180.0f;
    float const      yaw   = rotation.y * std::numbers::pi_v<float> / 180.0f;
    FmodVector const look{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    float const      dot = look.x * sample.forward.x + look.y * sample.forward.y + look.z * sample.forward.z;
    float const      dx = sample.pos.x - position.x, dy = sample.pos.y - position.y, dz = sample.pos.z - position.z;

    auto&       replay     = replay::ReplaySession::getInstance();
    auto        export_    = replay.exportCameraViewpoint();
    std::string exportPart = "none";
    if (export_) {
        exportPart = std::to_string(export_->x) + "," + std::to_string(export_->y) + "," + std::to_string(export_->z)
                   + " pitch=" + std::to_string(export_->pitch) + " yaw=" + std::to_string(export_->yaw);
    }

    ++gListenerLines;
    logger().info(
        "Audio probe listener bucket={} follow={} tick={} listener#{} calls={} age={}ms thread={} "
        "pos=({:.3f},{:.3f},{:.3f}) vel=({:.3f},{:.3f},{:.3f}) fwd=({:.3f},{:.3f},{:.3f}) up=({:.3f},{:.3f},{:.3f}) "
        "player=({:.3f},{:.3f},{:.3f}) rot=(pitch {:.2f}, yaw {:.2f}) delta=({:.3f},{:.3f},{:.3f}) lookDot={:.4f} "
        "exportCamera={}",
        BucketNames[bucket],
        replay.isActive() && replay.isFollowingRecordedPlayer(),
        replay.isActive() ? replay.getAppliedReplayTick() : -1,
        sample.listener,
        sample.calls,
        msSince(sample.at),
        sample.threadId,
        sample.pos.x,
        sample.pos.y,
        sample.pos.z,
        sample.vel.x,
        sample.vel.y,
        sample.vel.z,
        sample.forward.x,
        sample.forward.y,
        sample.forward.z,
        sample.up.x,
        sample.up.y,
        sample.up.z,
        position.x,
        position.y,
        position.z,
        rotation.x,
        rotation.y,
        dx,
        dy,
        dz,
        dot,
        exportPart
    );
}

template <class Detour>
bool installHook(void* target, Detour detour, ll::memory::FuncPtr& original, bool& installed, char const* name) {
    if (installed) return true;
    if (!target) {
        logger().warn("Audio probe: {} export not found", name);
        return false;
    }
    if (ll::memory::hook(target, ll::memory::toFuncPtr(detour), &original, ll::memory::HookPriority::Normal) != 0) {
        logger().warn("Audio probe: unable to hook {}", name);
        return false;
    }
    installed = true;
    return true;
}

template <class Detour>
void removeHook(void* target, Detour detour, bool& installed) {
    if (installed && ll::memory::unhook(target, ll::memory::toFuncPtr(detour))) installed = false;
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
        logger().warn("Audio probe: {} RVA is outside this game image", name);
        return nullptr;
    }
    uintptr_t const slot = *reinterpret_cast<uintptr_t const*>(base + slotRva);
    if (slot != base + fnRva) {
        logger().warn("Audio probe: {} vtable slot mismatch (slot rva=0x{:X}); unsupported game build", name, slot - base);
        return nullptr;
    }
    return reinterpret_cast<void*>(slot);
}

// The stream pause thunk is checked before hooking so an unexpected game build is rejected.
void* resolveStreamPause() {
    auto const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto const size = gameImageSize(base);
    if (StreamPauseThunkRva + 9 > size || StreamPauseRva >= size) return nullptr;
    auto const*                       thunk    = reinterpret_cast<uint8_t const*>(base + StreamPauseThunkRva);
    constexpr std::array<uint8_t, 5> Expected{0x48, 0x83, 0xC1, 0x18, 0xE9};
    if (std::memcmp(thunk, Expected.data(), Expected.size()) != 0) {
        logger().warn("Audio probe: stream pause thunk bytes differ; unsupported game build");
        return nullptr;
    }
    int32_t rel = 0;
    std::memcpy(&rel, thunk + 5, sizeof(rel));
    uintptr_t const target = base + StreamPauseThunkRva + 9 + intptr_t(rel);
    if (target != base + StreamPauseRva) {
        logger().warn("Audio probe: stream pause thunk target mismatch (rva=0x{:X})", target - base);
        return nullptr;
    }
    return reinterpret_cast<void*>(target);
}

} // namespace

bool install() {
    HMODULE const fmod = GetModuleHandleW(L"fmod.dll");
    if (!fmod) {
        logger().warn("Audio probe: fmod.dll is not loaded");
        return false;
    }
    resolve(fmod, "FMOD_System_GetVersion", gApi.getVersion);
    resolve(fmod, "FMOD_System_GetOutput", gApi.getOutput);
    resolve(fmod, "FMOD_System_SetOutput", gApi.setOutput);
    resolve(fmod, "FMOD_System_GetDriver", gApi.getDriver);
    resolve(fmod, "FMOD_System_SetDriver", gApi.setDriver);
    resolve(fmod, "FMOD_System_GetSoftwareFormat", gApi.getSoftwareFormat);
    resolve(fmod, "FMOD_System_GetDSPBufferSize", gApi.getDSPBufferSize);
    resolve(fmod, "FMOD_System_GetChannelsPlaying", gApi.getChannelsPlaying);
    resolve(fmod, "FMOD_System_Get3DSettings", gApi.get3DSettings);
    resolve(fmod, "FMOD_System_Get3DNumListeners", gApi.get3DNumListeners);
    resolve(fmod, "FMOD_System_GetMasterChannelGroup", gApi.getMasterChannelGroup);
    resolve(fmod, "FMOD_ChannelGroup_GetName", gApi.groupGetName);
    resolve(fmod, "FMOD_ChannelGroup_GetNumGroups", gApi.groupGetNumGroups);
    resolve(fmod, "FMOD_ChannelGroup_GetGroup", gApi.groupGetGroup);
    resolve(fmod, "FMOD_ChannelGroup_GetVolume", gApi.groupGetVolume);
    resolve(fmod, "FMOD_ChannelGroup_GetMute", gApi.groupGetMute);
    resolve(fmod, "FMOD_ChannelGroup_GetPaused", gApi.groupGetPaused);
    resolve(fmod, "FMOD_ChannelGroup_GetNumChannels", gApi.groupGetNumChannels);
    resolve(fmod, "FMOD_ChannelGroup_GetNumDSPs", gApi.groupGetNumDSPs);
    resolve(fmod, "FMOD_ChannelGroup_GetDSPIndex", gApi.groupGetDSPIndex);
    resolve(fmod, "FMOD_Sound_GetName", gApi.soundGetName);
    resolve(fmod, "FMOD_System_CreateDSP", gApi.createDSP);
    resolve(fmod, "FMOD_ChannelGroup_AddDSP", gApi.groupAddDSP);
    resolve(fmod, "FMOD_ChannelGroup_SetMute", gApi.groupSetMute);
    resolve(fmod, "FMOD_ChannelGroup_Stop", gApi.groupStop);
    resolve(fmod, "FMOD_ChannelGroup_RemoveDSP", gApi.groupRemoveDSP);
    resolve(fmod, "FMOD_DSP_Release", gApi.dspRelease);
    resolve(fmod, "FMOD_System_Close", gApi.systemClose);

    gHooks.update = reinterpret_cast<void*>(GetProcAddress(fmod, "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ"));
    gHooks.init   = reinterpret_cast<void*>(GetProcAddress(fmod, "?init@System@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAX@Z"));
    gHooks.setOutput = reinterpret_cast<void*>(
        GetProcAddress(fmod, "?setOutput@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z")
    );
    gHooks.playSound   = reinterpret_cast<void*>(GetProcAddress(
        fmod,
        "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z"
    ));
    gHooks.setListener = reinterpret_cast<void*>(
        GetProcAddress(fmod, "?set3DListenerAttributes@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_VECTOR@@000@Z")
    );

    bool const update = installHook(gHooks.update, &updateDetour, gOriginalUpdate, gHooks.updateInstalled, "update");
    bool const init   = installHook(gHooks.init, &initDetour, gOriginalInit, gHooks.initInstalled, "init");
    bool const setOutput =
        installHook(gHooks.setOutput, &setOutputDetour, gOriginalSetOutput, gHooks.setOutputInstalled, "setOutput");
    bool const play =
        installHook(gHooks.playSound, &playSoundDetour, gOriginalPlaySound, gHooks.playSoundInstalled, "playSound");
    bool const listener = installHook(
        gHooks.setListener,
        &setListenerDetour,
        gOriginalSetListener,
        gHooks.setListenerInstalled,
        "set3DListenerAttributes"
    );
    logger().info(
        "Audio probe hooks: update={} init={} setOutput={} playSound={} set3DListener={} capi={}/{}/{}",
        update,
        init,
        setOutput,
        play,
        listener,
        gApi.getVersion != nullptr,
        gApi.getMasterChannelGroup != nullptr,
        gApi.soundGetName != nullptr
    );
    // SoundSystemFMOD::update(float) hook is required for export audio (drives unpause); warn but don't block the probe.
    gUpdateFloatTarget = resolveSoundSystemSlot(UpdateFloatVtableSlot, UpdateFloatRva, "update(float)");
    bool const updateFloat =
        installHook(gUpdateFloatTarget, &updateFloatDetour, gOriginalUpdateFloat, gUpdateFloatInstalled, "update(float)");
    if (!updateFloat) logger().warn("Audio probe: SoundSystemFMOD::update(float) hook failed; export audio will not unpause");
    gEnableHook.target          = resolveSoundSystemSlot(EnableVtableSlot, EnableRva, "enable");
    gSetOutputDeviceHook.target = resolveSoundSystemSlot(SetOutputDeviceVtableSlot, SetOutputDeviceRva, "setOutputDevice");
    gStreamPauseHook.target     = resolveStreamPause();
    bool const guards =
        installHook(gEnableHook.target, &enableDetour, gEnableHook.original, gEnableHook.installed, "enable")
        & installHook(
            gSetOutputDeviceHook.target,
            &setOutputDeviceDetour,
            gSetOutputDeviceHook.original,
            gSetOutputDeviceHook.installed,
            "setOutputDevice"
        )
        & installHook(
            gStreamPauseHook.target,
            &streamPauseDetour,
            gStreamPauseHook.original,
            gStreamPauseHook.installed,
            "stream pause"
        );
    if (!guards) logger().warn("Audio probe: output guard hooks incomplete; export audio disabled");
    return update && play && listener;
}

void uninstall() {
    gPolicy = {};
    gGateUpdateFloat.store(false, std::memory_order_relaxed);
    removeHook(gStreamPauseHook.target, &streamPauseDetour, gStreamPauseHook.installed);
    removeHook(gSetOutputDeviceHook.target, &setOutputDeviceDetour, gSetOutputDeviceHook.installed);
    removeHook(gEnableHook.target, &enableDetour, gEnableHook.installed);
    removeHook(gUpdateFloatTarget, &updateFloatDetour, gUpdateFloatInstalled);
    removeHook(gHooks.setListener, &setListenerDetour, gHooks.setListenerInstalled);
    removeHook(gHooks.playSound, &playSoundDetour, gHooks.playSoundInstalled);
    removeHook(gHooks.setOutput, &setOutputDetour, gHooks.setOutputInstalled);
    removeHook(gHooks.init, &initDetour, gHooks.initInstalled);
    removeHook(gHooks.update, &updateDetour, gHooks.updateInstalled);
}

void setPolicy(Policy policy) { gPolicy = std::move(policy); }

OrigUpdateFn origUpdateFn() { return reinterpret_cast<OrigUpdateFn>(gOriginalUpdate); }

void* currentSystem() { return gSystem.load(); }

OrigUpdateFloatFn origUpdateFloatFn() { return reinterpret_cast<OrigUpdateFloatFn>(gOriginalUpdateFloat); }

void* currentSoundSystem() { return gSoundSystem.load(); }

bool shouldGateUpdateFloat() { return gGateUpdateFloat.load(std::memory_order_relaxed); }

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

bool outputGuardsInstalled() {
    return gEnableHook.installed && gSetOutputDeviceHook.installed && gStreamPauseHook.installed;
}

// Internal surface for playback::audio::exportcap (same module, not in the public header).
namespace exportcap_internal {
playback::audio::fmod::CApi const& api() { return gApi; }
} // namespace exportcap_internal

void onClientUpdate(ClientInstance& client) {
    gMainThreadId.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);
    Bucket const bucket   = computeBucket();
    auto const   previous = static_cast<Bucket>(gBucket.exchange(bucket));
    if (previous != bucket) {
        logger().info(
            "Audio probe bucket {} -> {} plays[{}]={} updates={}",
            BucketNames[previous],
            BucketNames[bucket],
            BucketNames[previous],
            gPlayCalls[previous].load(),
            gUpdateCalls.load()
        );
    }
    logPlaySummary(bucket);
    logListener(client, bucket);
}

} // namespace playback::audio::probe
