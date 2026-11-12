#include "OfflineRenderTimeHooks.h"

#include "playback/Playback.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <mutex>
#include <string>

namespace playback::exporting {

namespace {

// Client RVAs (1.26.20.04). Render code reads QPC through MSVCP140's _Query_perf_counter: the import thunk is
// jmp [slot], and each reader calls the thunk with a 5-byte E8 call.
constexpr uintptr_t QpcImportSlotRva = 0xB6D28E0;
constexpr uintptr_t QpcThunkRva      = 0xAC5BD40;
constexpr size_t    SiteCount        = 8;
constexpr size_t    ClockCount       = 3;
constexpr size_t    VibrantClock     = 0;
constexpr size_t    RayClock         = 1;
constexpr size_t    InterfaceClock   = 2;
// Return addresses of those calls: Vibrant Visuals exposure dt, ray tracing frame time, ray tracing start time and ray
// tracing shader seconds, then the reads a screen times its interface animations with: two in ScreenView's
// constructor, ScreenView::onFocusGained and ScreenView::render.
constexpr std::array<uintptr_t, SiteCount> SiteReturnRvas{
    0x9300A86,
    0xA303CE3,
    0xA30850D,
    0xA30AE79,
    0x4A6B59,
    0x4A6C32,
    0x4AD46E,
    0x4B0E75,
};
constexpr std::array<size_t, SiteCount> SiteClocks{
    VibrantClock,
    RayClock,
    RayClock,
    RayClock,
    InterfaceClock,
    InterfaceClock,
    InterfaceClock,
    InterfaceClock,
};
// The site that reads each clock on every frame.
constexpr std::array<size_t, ClockCount> FrameSites{0, 1, 7};

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
    bool                             captureSeen{};
    uint64_t                         sinceCapture{};
    uint64_t                         captureGaps{};
    uint64_t                         extraPerCaptureMin{std::numeric_limits<uint64_t>::max()};
    uint64_t                         extraPerCaptureMax{};
};

std::atomic_bool                             gActive{false};
std::atomic<QueryCounterFn>                  gOriginal{nullptr};
uintptr_t                                    gBase{};
std::array<RendererClock, ClockCount>        gClocks;
std::array<std::atomic<uint64_t>, SiteCount> gSiteHits{};
std::atomic<uint64_t>                        gOtherHits{};

// Main thread state.
std::mutex   gStateMutex;
InstallState gInstall{InstallState::NotTried};
std::string  gVerify{"untried"};
bool         gSessionOpen{};
int64_t      gFrequency{};
// The real counter at the last frame, which is where a frame the game draws for itself measures its time from.
int64_t   gLastFrameCounter{};
TimeStats gStats;

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

QueryCounterFn* importSlot() { return reinterpret_cast<QueryCounterFn*>(gBase + QpcImportSlotRva); }

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
    if (QpcImportSlotRva + sizeof(void*) > imageSize || QpcThunkRva + 6 > imageSize) return "image-size";
    for (auto const rva : SiteReturnRvas) {
        if (rva < 5 || rva > imageSize) return "image-size";
    }

    auto const runtime = GetModuleHandleW(L"msvcp140.dll");
    if (!runtime) return "msvcp140";
    auto const expected = reinterpret_cast<QueryCounterFn>(GetProcAddress(runtime, "_Query_perf_counter"));
    if (!expected) return "export";
    auto* const slot = reinterpret_cast<QueryCounterFn*>(base + QpcImportSlotRva);
    if (*slot != expected) return "slot";

    auto const thunk = reinterpret_cast<uint8_t const*>(base + QpcThunkRva);
    if (thunk[0] != 0xFF || thunk[1] != 0x25
        || relativeTarget(base + QpcThunkRva + 6, thunk + 2) != base + QpcImportSlotRva)
        return "thunk";
    for (size_t index = 0; index < SiteReturnRvas.size(); ++index) {
        auto const returnAddress = base + SiteReturnRvas[index];
        auto const call          = reinterpret_cast<uint8_t const*>(returnAddress - 5);
        if (call[0] != 0xE8 || relativeTarget(returnAddress, call + 1) != base + QpcThunkRva)
            return "site" + std::to_string(index);
    }

    gBase = base;
    gOriginal.store(expected, std::memory_order_release);
    if (!writeSlot(slot, expected, &queryCounterDetour)) return "protect";
    return "ok";
}

void stepClockTicks(size_t index, int64_t ticks) {
    if (ticks <= 0) return;
    auto& clock = gClocks[index];
    // A step nobody read means the render thread fell a whole sample behind the main thread.
    bool const everRead = gSiteHits[FrameSites[index]].load(std::memory_order_relaxed) != 0;
    if (clock.readsSinceStep.exchange(0, std::memory_order_relaxed) == 0 && everRead) ++gStats.skipped[index];
    clock.value.fetch_add(ticks, std::memory_order_acq_rel);
    gStats.advancedTicks[index] += ticks;
}

void stepClock(size_t index, double seconds) {
    stepClockTicks(index, static_cast<int64_t>(std::llround(seconds * static_cast<double>(gFrequency))));
}

double ticksToMilliseconds(int64_t ticks) {
    return gFrequency == 0 ? 0.0 : static_cast<double>(ticks) * 1000.0 / static_cast<double>(gFrequency);
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
        "Offline renderer clock: installed={}, verify={}, hits(vv={}, rtFrame={}, rtInit={}, rtConst={}, uiNew={}, "
        "uiFocus={}, uiFrame={}), other={}, adv(n={}, vvMs={:.1f}, rtMs={:.1f}, uiMs={:.1f}, expectMs={:.1f}, "
        "nativeFrames={}), tok(cap={}, warm={}, extra={}), "
        "renders(cap={}, warm={}, extra={}, keep={}), retry={}, extraPerCap(gaps={}, min={}, max={}), "
        "skipped(vv={}, rt={}), off(vvMs={:.1f}, rtMs={:.1f}, uiMs={:.1f})",
        installName(gInstall),
        gVerify,
        gSiteHits[0].load(std::memory_order_relaxed),
        gSiteHits[1].load(std::memory_order_relaxed),
        gSiteHits[2].load(std::memory_order_relaxed),
        gSiteHits[3].load(std::memory_order_relaxed),
        gSiteHits[4].load(std::memory_order_relaxed) + gSiteHits[5].load(std::memory_order_relaxed),
        gSiteHits[6].load(std::memory_order_relaxed),
        gSiteHits[7].load(std::memory_order_relaxed),
        gOtherHits.load(std::memory_order_relaxed),
        stats.advances,
        ticksToMilliseconds(stats.advancedTicks[VibrantClock]),
        ticksToMilliseconds(stats.advancedTicks[RayClock]),
        ticksToMilliseconds(stats.advancedTicks[InterfaceClock]),
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
        ticksToMilliseconds(gClocks[RayClock].offset.load(std::memory_order_relaxed)),
        ticksToMilliseconds(gClocks[InterfaceClock].offset.load(std::memory_order_relaxed))
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

    if (gInstall == InstallState::NotTried) {
        gVerify  = installDetour();
        gInstall = gVerify == "ok" ? InstallState::Installed : InstallState::Rejected;
        if (gInstall == InstallState::Rejected) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Export timing of ray tracing, Vibrant Visuals and interface animations is unavailable on this game "
                "build (check={}); they follow the wall clock",
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
    gLastFrameCounter = original();
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
}

void advanceOfflineRenderTime(OfflineRenderClockKind kind, float deltaTicks, bool firstRender) {
    std::scoped_lock lock(gStateMutex);
    if (!gActive.load(std::memory_order_relaxed)) return;
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
    gLastFrameCounter = gOriginal.load(std::memory_order_acquire)();

    double const seconds = static_cast<double>(deltaTicks) / 20.0;
    if (!(seconds > 0.0)) return;

    ++gStats.advances;
    gStats.expectedSeconds += seconds;
    stepClock(VibrantClock, std::min(seconds, VibrantMaxStepSeconds));
    stepClock(RayClock, seconds);
    stepClock(InterfaceClock, seconds);
}

void advanceOfflineRenderTimeForNativeFrame() {
    std::scoped_lock lock(gStateMutex);
    if (!gActive.load(std::memory_order_relaxed)) return;
    auto const now = gOriginal.load(std::memory_order_acquire)();
    ++gStats.nativeFrames;
    stepClockTicks(InterfaceClock, now - gLastFrameCounter);
    gLastFrameCounter = now;
}

void releaseOfflineRenderTimeHooks() {
    std::scoped_lock lock(gStateMutex);
    if (gInstall != InstallState::Installed) return;
    gActive.store(false, std::memory_order_release);
    gInstall = InstallState::Released;
    if (writeSlot(importSlot(), &queryCounterDetour, gOriginal.load(std::memory_order_acquire))) {
        // A later enable may patch again.
        gInstall = InstallState::NotTried;
    } else {
        Playback::getInstance().getSelf().getLogger().warn(
            "The renderer clock import changed after Playback patched it; left it in place"
        );
    }
}

} // namespace playback::exporting
