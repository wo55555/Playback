#include "GraphicsSwitchTrace.h"

#include <Windows.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <mutex>

namespace playback::editor::graphics {
namespace {

constexpr uint32_t    MaxEventsPerSwitch = 256;
constexpr uint32_t    SubmitBudget       = 16;
std::mutex            gMutex;
HANDLE                gFile = INVALID_HANDLE_VALUE;
std::atomic<uint64_t> gCurrent{};
uint64_t              gNext{};
uint64_t              gFirst{};
uint64_t              gSequence{};
uint32_t              gEvents{};
uint32_t              gSubmissions{};
uint32_t              gRemainingSubmissions{};

bool writeRecord(uint64_t transition, std::string_view event, std::string_view detail) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char       line[2048];
    auto const length = std::snprintf(
        line,
        sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u seq=%" PRIu64 " switch=%" PRIu64 " thread=%lu event=%.*s %.*s\r\n",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        ++gSequence,
        transition,
        GetCurrentThreadId(),
        static_cast<int>(event.size()),
        event.data(),
        static_cast<int>(detail.size()),
        detail.empty() ? "" : detail.data()
    );
    if (length < 0 || static_cast<size_t>(length) >= sizeof(line)) return false;
    DWORD written{};
    if (!WriteFile(gFile, line, static_cast<DWORD>(length), &written, nullptr) || written != static_cast<DWORD>(length)
        || !FlushFileBuffers(gFile)) {
        CloseHandle(gFile);
        gFile = INVALID_HANDLE_VALUE;
        gCurrent.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

} // namespace

bool openGraphicsSwitchTrace(std::filesystem::path const& path) noexcept try {
    std::scoped_lock lock(gMutex);
    if (gFile != INVALID_HANDLE_VALUE) return false;
    gFile =
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (gFile == INVALID_HANDLE_VALUE) return false;
    gFirst    = gNext + 1;
    gSequence = 0;
    gEvents   = 0;
    gCurrent.store(0, std::memory_order_release);
    return writeRecord(0, "JournalOpened", "pointers=opaque observations-not-GPU-identity");
} catch (...) {
    return false;
}

void closeGraphicsSwitchTrace() noexcept {
    std::scoped_lock lock(gMutex);
    gCurrent.store(0, std::memory_order_release);
    if (gFile != INVALID_HANDLE_VALUE) {
        CloseHandle(gFile);
        gFile = INVALID_HANDLE_VALUE;
    }
}

uint64_t beginGraphicsSwitchTrace(int previous, int requested) noexcept {
    std::scoped_lock lock(gMutex);
    if (gFile == INVALID_HANDLE_VALUE) return 0;
    auto const transition = ++gNext;
    gEvents               = 0;
    gSubmissions          = 0;
    gRemainingSubmissions = SubmitBudget;
    gCurrent.store(transition, std::memory_order_release);
    char detail[80];
    std::snprintf(detail, sizeof(detail), "from=%d requested=%d", previous, requested);
    return writeRecord(transition, "ModeSetter.enter", detail) ? transition : 0;
}

uint64_t currentGraphicsSwitchTrace() noexcept { return gCurrent.load(std::memory_order_acquire); }

GraphicsSwitchSubmitTicket claimGraphicsSwitchSubmitTrace() noexcept {
    if (!currentGraphicsSwitchTrace()) return {};
    std::scoped_lock lock(gMutex);
    auto const       transition = gCurrent.load(std::memory_order_acquire);
    if (!transition || gRemainingSubmissions == 0) return {};
    --gRemainingSubmissions;
    return {transition, ++gSubmissions};
}

void rearmGraphicsSwitchSubmitTrace(uint64_t transition) noexcept {
    if (!transition) return;
    std::scoped_lock lock(gMutex);
    if (transition == gCurrent.load(std::memory_order_acquire)) gRemainingSubmissions = SubmitBudget;
}

void recordGraphicsSwitchTrace(uint64_t transition, std::string_view event, std::string_view detail) noexcept {
    if (!transition) return;
    std::scoped_lock lock(gMutex);
    if (gFile == INVALID_HANDLE_VALUE || transition < gFirst || transition > gNext || gEvents >= MaxEventsPerSwitch)
        return;
    // Keep the entry ticket on late exits instead of relabelling them as the newest switch.
    if (++gEvents == MaxEventsPerSwitch) {
        (void)writeRecord(transition, "WindowLimit", "further-events-suppressed-until-next-mode-request");
        gCurrent.store(0, std::memory_order_release);
        return;
    }
    (void)writeRecord(transition, event, detail);
}

} // namespace playback::editor::graphics