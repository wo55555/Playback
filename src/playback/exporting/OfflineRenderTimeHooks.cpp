#include "OfflineRenderTimeHooks.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/particlesystem/particle/ParticleSystemEngine.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace playback::exporting {

namespace {

constexpr size_t SiteCount    = 4;
constexpr size_t ClockCount   = 2;
constexpr size_t VibrantClock = 0;
constexpr size_t RayClock     = 1;
// The renderers read the performance counter through MSVCP140's _Query_perf_counter import, and none of the functions
// that do has a symbol, so the reads are told apart by where they return to. Client RVAs (1.26.60.05): Vibrant
// Visuals exposure dt, ray tracing frame time, ray tracing start time and ray tracing shader seconds.
constexpr std::array<uintptr_t, SiteCount> SiteReturnRvas{
    0x562D49E,
    0xE84189F,
    0xE83F8B1,
    0xE8437FD,
};
constexpr std::array<size_t, SiteCount> SiteClocks{
    VibrantClock,
    RayClock,
    RayClock,
    RayClock,
};
// The site that reads each clock on every frame.
constexpr std::array<size_t, ClockCount> FrameSites{0, 1};

// Exposure adaptation snaps on a large dt; timelapse frames would otherwise jump it every frame.
constexpr double VibrantMaxStepSeconds = 0.1;

using QueryCounterFn = long long(__cdecl*)();

struct RendererClock {
    // The value the sites read while an export is open.
    std::atomic<int64_t> value{};
    // Added to the real counter afterwards, so the renderers never see their time jump or run backwards.
    std::atomic<int64_t>  offset{};
    std::atomic<uint64_t> readsSinceStep{};
};

enum class InstallState : uint8_t { NotTried, Installed, Rejected, Released };

struct TimeStats {
    std::array<uint64_t, 4>          tokens{};
    std::array<uint64_t, 4>          renders{};
    uint64_t                         advances{};
    double                           expectedSeconds{};
    std::array<int64_t, ClockCount>  advancedTicks{};
    std::array<uint64_t, ClockCount> skipped{};
    uint64_t                         nativeFrames{};
    uint64_t                         screenRenders{};
    bool                             captureSeen{};
    uint64_t                         sinceCapture{};
    uint64_t                         captureGaps{};
    uint64_t                         extraPerCaptureMin{std::numeric_limits<uint64_t>::max()};
    uint64_t                         extraPerCaptureMax{};
};

std::atomic_bool                             gActive{false};
std::atomic<QueryCounterFn>                  gOriginal{nullptr};
uintptr_t                                    gBase{};
QueryCounterFn*                              gSlot{};
std::array<RendererClock, ClockCount>        gClocks;
std::array<std::atomic<uint64_t>, SiteCount> gSiteHits{};
std::atomic<uint64_t>                        gOtherHits{};

// Main thread state.
std::mutex   gStateMutex;
InstallState gInstall{InstallState::NotTried};
std::string  gVerify{"untried"};
bool         gSessionOpen{};
int64_t      gFrequency{};
bool         gScreenHookInstalled{};
bool         gParticleHookInstalled{};
// The replay time the screens have been given in this export, and where on it each screen last rendered.
std::chrono::nanoseconds                                        gInterfaceTime{};
std::unordered_map<ScreenView const*, std::chrono::nanoseconds> gScreenTimes;
// The replay time the render in progress adds to the scene; empty outside the renders of an export.
std::optional<std::chrono::nanoseconds> gRenderSceneStep;
// When the last frame was drawn, which is where a frame the game draws for itself measures its time from.
std::chrono::steady_clock::time_point gLastFrameAt{};
TimeStats                             gStats;

long long __cdecl queryCounterDetour() {
    auto const original = gOriginal.load(std::memory_order_acquire);
    auto const rva      = reinterpret_cast<uintptr_t>(_ReturnAddress()) - gBase;
    size_t     site     = SiteReturnRvas.size();
    for (size_t index = 0; index < SiteReturnRvas.size(); ++index) {
        if (rva == SiteReturnRvas[index]) {
            site = index;
            break;
        }
    }
    bool const active = gActive.load(std::memory_order_acquire);
    if (site == SiteReturnRvas.size()) {
        if (active) gOtherHits.fetch_add(1, std::memory_order_relaxed);
        return original();
    }
    auto& clock = gClocks[SiteClocks[site]];
    if (active) {
        gSiteHits[site].fetch_add(1, std::memory_order_relaxed);
        clock.readsSinceStep.fetch_add(1, std::memory_order_relaxed);
        return clock.value.load(std::memory_order_acquire);
    }
    return original() + clock.offset.load(std::memory_order_acquire);
}

uintptr_t relativeTarget(uintptr_t next, uint8_t const* displacement) {
    int32_t relative{};
    std::memcpy(&relative, displacement, sizeof(relative));
    return next + static_cast<uintptr_t>(static_cast<intptr_t>(relative));
}

// The game's import slot for MSVCP140's _Query_perf_counter.
QueryCounterFn* findImportSlot(uintptr_t base, IMAGE_NT_HEADERS64 const& nt) {
    auto const& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return nullptr;
    auto const* library = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(base + directory.VirtualAddress);
    for (; library->Name != 0; ++library) {
        if (_stricmp(reinterpret_cast<char const*>(base + library->Name), "MSVCP140.dll") != 0) continue;
        if (library->OriginalFirstThunk == 0) return nullptr;
        auto const* name = reinterpret_cast<IMAGE_THUNK_DATA64 const*>(base + library->OriginalFirstThunk);
        auto*       slot = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + library->FirstThunk);
        for (; name->u1.AddressOfData != 0; ++name, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL64(name->u1.Ordinal)) continue;
            auto const* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME const*>(base + name->u1.AddressOfData);
            if (std::strcmp(import->Name, "_Query_perf_counter") == 0) {
                return reinterpret_cast<QueryCounterFn*>(&slot->u1.Function);
            }
        }
    }
    return nullptr;
}

// Whether the call that returns here reads the slot: call [slot], or a call to a jmp [slot] thunk.
bool callsImportSlot(uintptr_t base, uintptr_t imageSize, uintptr_t returnAddress, uintptr_t slot) {
    auto const direct = reinterpret_cast<uint8_t const*>(returnAddress - 6);
    if (direct[0] == 0xFF && direct[1] == 0x15 && relativeTarget(returnAddress, direct + 2) == slot) return true;
    auto const call = reinterpret_cast<uint8_t const*>(returnAddress - 5);
    if (call[0] != 0xE8) return false;
    auto const thunk = relativeTarget(returnAddress, call + 1);
    if (thunk < base || thunk - base + 6 > imageSize) return false;
    auto const jump = reinterpret_cast<uint8_t const*>(thunk);
    return jump[0] == 0xFF && jump[1] == 0x25 && relativeTarget(thunk + 6, jump + 2) == slot;
}

bool writeSlot(QueryCounterFn* slot, QueryCounterFn expected, QueryCounterFn replacement) {
    DWORD previous{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    auto const old = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot),
        reinterpret_cast<PVOID>(replacement),
        reinterpret_cast<PVOID>(expected)
    );
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    return old == reinterpret_cast<PVOID>(expected);
}

// Every byte the detour relies on is checked first; any other game build keeps the wall clock.
std::string installDetour() {
    auto const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto const dos  = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return "image";
    auto const nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + static_cast<uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) return "image";
    uintptr_t const imageSize = nt->OptionalHeader.SizeOfImage;
    for (auto const rva : SiteReturnRvas) {
        if (rva < 6 || rva > imageSize) return "image-size";
    }

    auto const runtime = GetModuleHandleW(L"msvcp140.dll");
    if (!runtime) return "msvcp140";
    auto const expected = reinterpret_cast<QueryCounterFn>(GetProcAddress(runtime, "_Query_perf_counter"));
    if (!expected) return "export";
    auto* const slot = findImportSlot(base, *nt);
    if (!slot) return "import";
    if (*slot != expected) return "slot";

    for (size_t index = 0; index < SiteReturnRvas.size(); ++index) {
        if (!callsImportSlot(base, imageSize, base + SiteReturnRvas[index], reinterpret_cast<uintptr_t>(slot))) {
            return "site" + std::to_string(index);
        }
    }

    gBase = base;
    gSlot = slot;
    gOriginal.store(expected, std::memory_order_release);
    if (!writeSlot(slot, expected, &queryCounterDetour)) return "protect";
    return "ok";
}

void stepClock(size_t index, double seconds) {
    auto const ticks = static_cast<int64_t>(std::llround(seconds * static_cast<double>(gFrequency)));
    if (ticks <= 0) return;
    auto& clock = gClocks[index];
    // A step nobody read means the render thread fell a whole sample behind the main thread.
    bool const everRead = gSiteHits[FrameSites[index]].load(std::memory_order_relaxed) != 0;
    if (clock.readsSinceStep.exchange(0, std::memory_order_relaxed) == 0 && everRead) ++gStats.skipped[index];
    clock.value.fetch_add(ticks, std::memory_order_acq_rel);
    gStats.advancedTicks[index] += ticks;
}

double ticksToMilliseconds(int64_t ticks) {
    return gFrequency == 0 ? 0.0 : static_cast<double>(ticks) * 1000.0 / static_cast<double>(gFrequency);
}

// The time a screen's animations move by on this render: what its frames have added since it last rendered.
std::optional<std::chrono::nanoseconds> takeScreenStep(ScreenView const* screen) {
    std::scoped_lock lock(gStateMutex);
    if (!gSessionOpen) return std::nullopt;
    ++gStats.screenRenders;
    auto const entry = gScreenTimes.try_emplace(screen, gInterfaceTime).first;
    auto const step  = gInterfaceTime - entry->second;
    entry->second    = gInterfaceTime;
    return step;
}

// A screen steps its animations by the wall-clock time since its previous render, which it keeps in mCurrentTime.
LL_TYPE_INSTANCE_HOOK(
    OfflineRenderScreenAnimationHook,
    ll::memory::HookPriority::Normal,
    ScreenView,
    &ScreenView::render,
    void,
    UIRenderContext& uiRenderContext
) {
    if (auto const step = takeScreenStep(this)) this->mCurrentTime = std::chrono::steady_clock::now() - *step;
    origin(uiRenderContext);
}

std::optional<std::chrono::nanoseconds> takeRenderSceneStep() {
    std::scoped_lock lock(gStateMutex);
    if (!gSessionOpen) return std::nullopt;
    // Only the first tick of a render gets the step; a second one in the same render adds nothing.
    return std::exchange(gRenderSceneStep, gRenderSceneStep ? std::optional{std::chrono::nanoseconds{}} : std::nullopt);
}

// The particle systems tick once per render and age by the wall-clock time since their previous tick, which they keep
// in mLastTickTime. Ticks made outside a render, as a seek makes them, keep their own time.
LL_TYPE_INSTANCE_HOOK(
    OfflineRenderParticleSystemHook,
    ll::memory::HookPriority::Normal,
    ParticleSystemEngine,
    &ParticleSystemEngine::tick,
    void,
    float a,
    bool  paused
) {
    if (auto const step = takeRenderSceneStep(); step && this->mLastTickTimeValid) {
        this->mLastTickTime = std::chrono::steady_clock::now() - *step;
    }
    origin(a, paused);
}

char const* installName(InstallState state) {
    switch (state) {
    case InstallState::NotTried:
        return "untried";
    case InstallState::Installed:
        return "yes";
    case InstallState::Rejected:
        return "rejected";
    case InstallState::Released:
        return "released";
    }
    return "unknown";
}

void reportSession() {
    auto const& stats   = gStats;
    auto const  capture = static_cast<size_t>(OfflineRenderClockKind::Capture);
    auto const  warmup  = static_cast<size_t>(OfflineRenderClockKind::Warmup);
    auto const  extra   = static_cast<size_t>(OfflineRenderClockKind::ExtraRender);
    auto const  keep    = static_cast<size_t>(OfflineRenderClockKind::KeepAlive);
    Playback::getInstance().getSelf().getLogger().debug(
        "Offline renderer clock: installed={}, verify={}, screens={}, hits(vv={}, rtFrame={}, rtInit={}, rtConst={}, "
        "uiFrame={}), other={}, adv(n={}, vvMs={:.1f}, rtMs={:.1f}, uiMs={:.1f}, expectMs={:.1f}, "
        "nativeFrames={}), tok(cap={}, warm={}, extra={}), "
        "renders(cap={}, warm={}, extra={}, keep={}), retry={}, extraPerCap(gaps={}, min={}, max={}), "
        "skipped(vv={}, rt={}), off(vvMs={:.1f}, rtMs={:.1f})",
        installName(gInstall),
        gVerify,
        gScreenHookInstalled,
        gSiteHits[0].load(std::memory_order_relaxed),
        gSiteHits[1].load(std::memory_order_relaxed),
        gSiteHits[2].load(std::memory_order_relaxed),
        gSiteHits[3].load(std::memory_order_relaxed),
        stats.screenRenders,
        gOtherHits.load(std::memory_order_relaxed),
        stats.advances,
        ticksToMilliseconds(stats.advancedTicks[VibrantClock]),
        ticksToMilliseconds(stats.advancedTicks[RayClock]),
        std::chrono::duration<double, std::milli>(gInterfaceTime).count(),
        stats.expectedSeconds * 1000.0,
        stats.nativeFrames,
        stats.tokens[capture],
        stats.tokens[warmup],
        stats.tokens[extra],
        stats.renders[capture],
        stats.renders[warmup],
        stats.renders[extra],
        stats.renders[keep],
        stats.renders[capture] - stats.tokens[capture],
        stats.captureGaps,
        stats.captureGaps == 0 ? 0 : stats.extraPerCaptureMin,
        stats.extraPerCaptureMax,
        stats.skipped[VibrantClock],
        stats.skipped[RayClock],
        ticksToMilliseconds(gClocks[VibrantClock].offset.load(std::memory_order_relaxed)),
        ticksToMilliseconds(gClocks[RayClock].offset.load(std::memory_order_relaxed))
    );
}

} // namespace

void beginOfflineRenderTime() {
    std::scoped_lock lock(gStateMutex);
    if (gSessionOpen) return;
    gSessionOpen = true;
    gStats       = {};
    for (auto& hits : gSiteHits) hits.store(0, std::memory_order_relaxed);
    gOtherHits.store(0, std::memory_order_relaxed);

    gInterfaceTime = {};
    gScreenTimes.clear();
    gLastFrameAt = std::chrono::steady_clock::now();
    gRenderSceneStep.reset();
    if (!gParticleHookInstalled) {
        gParticleHookInstalled = OfflineRenderParticleSystemHook::hook() == 0;
        if (!gParticleHookInstalled) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Export timing of particle systems is unavailable; they follow the wall clock"
            );
        }
    }
    if (!gScreenHookInstalled) {
        gScreenHookInstalled = OfflineRenderScreenAnimationHook::hook() == 0;
        if (!gScreenHookInstalled) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Export timing of interface animations is unavailable; they follow the wall clock"
            );
        }
    }

    if (gInstall == InstallState::NotTried) {
        gVerify  = installDetour();
        gInstall = gVerify == "ok" ? InstallState::Installed : InstallState::Rejected;
        if (gInstall == InstallState::Rejected) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Export timing of ray tracing and Vibrant Visuals is unavailable on this game build (check={}); they "
                "follow the wall clock",
                gVerify
            );
        }
    }
    if (gInstall != InstallState::Installed) return;

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    gFrequency          = frequency.QuadPart;
    auto const original = gOriginal.load(std::memory_order_acquire);
    // The lead covers reads that land between this store and the switch, so the clock never steps back.
    auto const lead = gFrequency / 1000;
    for (auto& clock : gClocks) {
        clock.value.store(original() + clock.offset.load(std::memory_order_relaxed) + lead, std::memory_order_relaxed);
        clock.readsSinceStep.store(0, std::memory_order_relaxed);
    }
    gActive.store(true, std::memory_order_release);
}

void endOfflineRenderTime() {
    std::scoped_lock lock(gStateMutex);
    if (!gSessionOpen) return;
    gSessionOpen = false;
    if (gActive.load(std::memory_order_relaxed)) {
        // Offsets land before the switch flips, so a read in between still returns at least the virtual value.
        auto const real = gOriginal.load(std::memory_order_acquire)();
        for (auto& clock : gClocks) {
            clock.offset.store(clock.value.load(std::memory_order_relaxed) - real, std::memory_order_release);
        }
        gActive.store(false, std::memory_order_release);
    }
    reportSession();
    // The screens kept the real time of their last render, so their next step is measured from there again.
    if (gScreenHookInstalled && OfflineRenderScreenAnimationHook::unhook()) gScreenHookInstalled = false;
    if (gParticleHookInstalled && OfflineRenderParticleSystemHook::unhook()) gParticleHookInstalled = false;
    gRenderSceneStep.reset();
    gScreenTimes.clear();
}

void advanceOfflineRenderTime(OfflineRenderClockKind kind, float deltaTicks, bool firstRender) {
    std::scoped_lock lock(gStateMutex);
    if (!gSessionOpen) return;
    auto const index = static_cast<size_t>(kind);
    ++gStats.renders[index];
    if (kind == OfflineRenderClockKind::Capture && firstRender) {
        if (gStats.captureSeen) {
            ++gStats.captureGaps;
            gStats.extraPerCaptureMin = std::min(gStats.extraPerCaptureMin, gStats.sinceCapture);
            gStats.extraPerCaptureMax = std::max(gStats.extraPerCaptureMax, gStats.sinceCapture);
        }
        gStats.captureSeen  = true;
        gStats.sinceCapture = 0;
    } else if (gStats.captureSeen) {
        ++gStats.sinceCapture;
    }
    if (firstRender) ++gStats.tokens[index];
    gLastFrameAt = std::chrono::steady_clock::now();

    double const seconds = static_cast<double>(deltaTicks) / 20.0;
    // The scene holds still through warm-up, so only the renderers' time runs there.
    gRenderSceneStep = kind != OfflineRenderClockKind::Warmup && seconds > 0.0
                         ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds))
                         : std::chrono::nanoseconds{};
    if (!(seconds > 0.0)) return;

    ++gStats.advances;
    gStats.expectedSeconds += seconds;
    gInterfaceTime += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds));
    if (!gActive.load(std::memory_order_relaxed)) return;
    stepClock(VibrantClock, std::min(seconds, VibrantMaxStepSeconds));
    stepClock(RayClock, seconds);
}

void finishOfflineRenderTime() {
    std::scoped_lock lock(gStateMutex);
    gRenderSceneStep.reset();
}

void advanceOfflineRenderTimeForNativeFrame() {
    std::scoped_lock lock(gStateMutex);
    if (!gSessionOpen) return;
    auto const now = std::chrono::steady_clock::now();
    ++gStats.nativeFrames;
    gInterfaceTime += now - gLastFrameAt;
    gLastFrameAt    = now;
}

void releaseOfflineRenderTimeHooks() {
    std::scoped_lock lock(gStateMutex);
    if (gInstall != InstallState::Installed) return;
    gActive.store(false, std::memory_order_release);
    gInstall = InstallState::Released;
    if (writeSlot(gSlot, &queryCounterDetour, gOriginal.load(std::memory_order_acquire))) {
        // A later enable may patch again.
        gInstall = InstallState::NotTried;
    } else {
        Playback::getInstance().getSelf().getLogger().warn(
            "The renderer clock import changed after Playback patched it; left it in place"
        );
    }
}

} // namespace playback::exporting
