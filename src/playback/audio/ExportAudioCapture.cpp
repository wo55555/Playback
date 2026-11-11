#include "ExportAudioCapture.h"

#include "AudioHooks.h"
#include "ExportAudioWav.h"
#include "FmodMinimal.h"

#include "playback/Playback.h"
#include "playback/exporting/ExportTypes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace playback::audio::exportcap {

namespace {

using namespace playback::audio::fmod;

// Non-real-time capture. For the export the game's FMOD output is switched to NOSOUND_NRT: there is no mixer thread,
// and each System::update mixes exactly one DSP block (512 samples at 48 kHz) on the calling thread. The export calls
// update until the frame ledger is satisfied, and a capture DSP at the master group's TAIL (before the fader, so user
// volume does not change the grab) copies every block. The original output and driver are restored at the end.
constexpr int64_t R = 48000;

constexpr int     FirstFrameFlushUpdates = 2;   // blocks mixed and dropped after Stop(master) so the ramp is not captured
constexpr int     ExtraUpdatesPerFrame   = 8;   // slack over need/512 before a frame is declared stalled
constexpr int64_t SummaryEvery           = 120; // accepted frames per summary line

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

enum class Phase : int {
    Discarding, // first-frame cleanup: PCM dropped
    Capturing,  // export frames: PCM queued for the WAV
};

std::atomic<bool> gArmed{false};
std::atomic<bool> gOwnsOutput{false}; // output switched to NRT; game update/enable/device calls are held back
std::atomic<int>  gPhase{int(Phase::Discarding)};
thread_local bool tDriving = false; // this thread is mixing for the export; let System::update through

CApi const*              gApi             = nullptr;
hooks::OrigUpdateFloatFn gOrigUpdateFloat = nullptr;
hooks::OrigUpdateFn      gOrigUpdate      = nullptr;

std::mutex gMutex;
void*      gSystem      = nullptr;
void*      gSoundSystem = nullptr;
void*      gMaster      = nullptr;
void*      gDsp         = nullptr;
bool       gCapturing   = false;
bool       gFirstFrame  = true;
bool       gWavOk       = false;
int        gSavedOutput = -1;
int        gSavedDriver = -1;

// Game calls deferred while the export owns the output; replayed after the restore.
std::mutex                            gPendingMutex;
std::optional<bool>                   gPendingEnable;
std::optional<std::array<uint8_t, 16>> gPendingDevice;

// Render-thread state (guarded by gMutex). Ledger units are samples per channel.
uint64_t gLastFrameIndex = 0;
int64_t  gAcceptedFrames = 0; // k
int64_t  gWritten        = 0; // samples written to the WAV
int64_t  gPadDebt        = 0; // samples padded with silence that a later block still owes; dropped when they arrive
int64_t  gFrameNum       = 60;
int64_t  gFrameDen       = 1;

WavWriter             gWav;
std::filesystem::path gSidecar;

std::mutex         gPcmMutex;
std::vector<float> gPcm; // stereo interleaved, captured but not yet written

// Counters for the summary lines. Window counters reset after each summary line.
std::atomic<int64_t> gInch1{0}, gInch2{0}, gInch6{0}, gInch8{0}, gInchOther{0};
std::atomic<int64_t> gLenMin{INT64_MAX}, gLenMax{0};
std::atomic<int64_t> gBlocks{0};

struct Window {
    int64_t firstFrame    = 1;
    int64_t updates       = 0; // update(float) drives
    int64_t mixUpdates    = 0; // extra bare System::update calls
    double  updMsMax      = 0;
    double  updMsSum      = 0;
    int64_t stalledFrames = 0;
    int64_t paddedSamples = 0;
    int     channelsPeak  = 0;
    float   peak          = 0;
    double  sumSq         = 0;
    int64_t sqCount       = 0;
};
Window  gWin;
int64_t gTotalMixUpdates    = 0;
int64_t gTotalStalledFrames = 0;
int64_t gTotalPadded        = 0;
int     gTotalChannelsPeak  = 0;
float   gTotalPeak          = 0;

void atomicMax(std::atomic<int64_t>& a, int64_t v) {
    int64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void atomicMin(std::atomic<int64_t>& a, int64_t v) {
    int64_t cur = a.load(std::memory_order_relaxed);
    while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

void countLayout(int inchannels, unsigned int length) {
    switch (inchannels) {
    case 1: gInch1.fetch_add(1, std::memory_order_relaxed); break;
    case 2: gInch2.fetch_add(1, std::memory_order_relaxed); break;
    case 6: gInch6.fetch_add(1, std::memory_order_relaxed); break;
    case 8: gInch8.fetch_add(1, std::memory_order_relaxed); break;
    default: gInchOther.fetch_add(1, std::memory_order_relaxed); break;
    }
    atomicMin(gLenMin, int64_t(length));
    atomicMax(gLenMax, int64_t(length));
    gBlocks.fetch_add(1, std::memory_order_relaxed);
}

// Downmix any input layout to stereo. FMOD channel order: FL FR FC LFE SL SR [BL BR]; LFE is dropped.
void pushPcm(float const* in, unsigned int length, int inchannels) {
    std::lock_guard lock(gPcmMutex);
    size_t const    base = gPcm.size();
    gPcm.resize(base + size_t(length) * 2);
    float* out = gPcm.data() + base;
    if (!in || inchannels <= 0) {
        std::memset(out, 0, size_t(length) * 2 * sizeof(float));
        return;
    }
    constexpr float k = 0.7071f;
    for (unsigned int i = 0; i < length; ++i) {
        float const* f = in + size_t(i) * inchannels;
        float        l = 0, r = 0;
        switch (inchannels) {
        case 1: l = r = f[0]; break;
        case 2: l = f[0]; r = f[1]; break;
        case 6:
            l = f[0] + k * (f[2] + f[4]);
            r = f[1] + k * (f[2] + f[5]);
            break;
        case 8:
            l = f[0] + k * (f[2] + f[4] + f[6]);
            r = f[1] + k * (f[2] + f[5] + f[7]);
            break;
        default:
            l = f[0];
            r = f[1];
            break;
        }
        out[i * 2 + 0] = l;
        out[i * 2 + 1] = r;
    }
}

// Runs inside System::update on the driving thread (NRT has no mixer thread). Output is zeroed: nothing is audible
// under NRT anyway, and it keeps the DSP silent if it outlives the restore by mistake.
FMOD_RESULT PB_F_CALLBACK
captureRead(FMOD_DSP_STATE*, float* in, float* out, unsigned int length, int inchannels, int* outchannels) {
    *outchannels = inchannels;
    if (out && inchannels > 0) std::memset(out, 0, size_t(length) * inchannels * sizeof(float));
    countLayout(inchannels, length);
    if (Phase(gPhase.load()) == Phase::Capturing) pushPcm(in, length, inchannels);
    return FMOD_OK;
}

// Without this, FMOD skips the read callback while all inputs are idle, so silent stretches would never reach the
// ledger.
FMOD_RESULT PB_F_CALLBACK
captureShouldProcess(FMOD_DSP_STATE*, FMOD_BOOL, unsigned int, FMOD_CHANNELMASK, int, FMOD_SPEAKERMODE) {
    return FMOD_OK;
}

void resetCounters() {
    gInch1 = gInch2 = gInch6 = gInch8 = gInchOther = 0;
    gLenMin                                       = INT64_MAX;
    gLenMax                                       = 0;
    gBlocks                                       = 0;
    gWin                                          = {};
    gTotalMixUpdates                              = 0;
    gTotalStalledFrames                           = 0;
    gTotalPadded                                  = 0;
    gTotalChannelsPeak                            = 0;
    gTotalPeak                                    = 0;
}

double toDb(double v) { return v > 1e-9 ? 20.0 * std::log10(v) : -180.0; }

int64_t queuedFrames() {
    std::lock_guard plock(gPcmMutex);
    return int64_t(gPcm.size() / 2);
}

void logWindowLocked(char const* tag) {
    if (gWin.updates == 0) return;
    double const rms = gWin.sqCount > 0 ? std::sqrt(gWin.sumSq / double(gWin.sqCount)) : 0.0;
    logger().info(
        "Export audio {} frames=[{},{}] updates={} mixUpdates={} updMsAvg={:.2f} updMsMax={:.2f} stalledFrames={} "
        "padded={} chPeak={} peakDb={:.1f} rmsDb={:.1f} blocks={} inch[1={},2={},6={},8={},other={}] len=[{},{}] "
        "queued={}",
        tag,
        gWin.firstFrame,
        gAcceptedFrames,
        gWin.updates,
        gWin.mixUpdates,
        gWin.updMsSum / double(gWin.updates),
        gWin.updMsMax,
        gWin.stalledFrames,
        gWin.paddedSamples,
        gWin.channelsPeak,
        toDb(gWin.peak),
        toDb(rms),
        gBlocks.load(),
        gInch1.load(),
        gInch2.load(),
        gInch6.load(),
        gInch8.load(),
        gInchOther.load(),
        gLenMin.load() == INT64_MAX ? 0 : gLenMin.load(),
        gLenMax.load(),
        queuedFrames()
    );
    gWin            = {};
    gWin.firstFrame = gAcceptedFrames + 1;
}

// One bare System::update on this thread: under NRT it mixes one block through the capture DSP.
void mixOneBlockLocked() {
    tDriving = true;
    gOrigUpdate(gSystem);
    tDriving = false;
}

void releaseDspLocked() {
    if (gDsp && gApi) {
        if (gMaster) gApi->groupRemoveDSP(gMaster, gDsp);
        gApi->dspRelease(gDsp);
    }
    gDsp       = nullptr;
    gCapturing = false;
    gPhase     = int(Phase::Discarding);
    std::lock_guard plock(gPcmMutex);
    gPcm.clear();
}

// Put the game's output and driver back (setOutput restarts on driver 0, so the driver is set after), then replay
// the enable/device calls the game made in the meantime.
void restoreOutputLocked() {
    if (!gOwnsOutput.exchange(false)) return;
    int output = -1, driver = -1;
    gApi->getOutput(gSystem, &output);
    int setOutputResult = 0, setDriverResult = 0;
    if (gSavedOutput >= 0 && output != gSavedOutput) setOutputResult = gApi->setOutput(gSystem, gSavedOutput);
    gApi->getDriver(gSystem, &driver);
    if (gSavedDriver >= 0 && driver != gSavedDriver) setDriverResult = gApi->setDriver(gSystem, gSavedDriver);
    int finalOutput = -1, finalDriver = -1;
    gApi->getOutput(gSystem, &finalOutput);
    gApi->getDriver(gSystem, &finalDriver);
    logger().info(
        "Export audio capture: output restored to {} driver {} (saved {}/{}, setOutput={}, setDriver={})",
        finalOutput,
        finalDriver,
        gSavedOutput,
        gSavedDriver,
        setOutputResult,
        setDriverResult
    );

    std::optional<bool>                    enable;
    std::optional<std::array<uint8_t, 16>> device;
    {
        std::lock_guard plock(gPendingMutex);
        enable = std::exchange(gPendingEnable, std::nullopt);
        device = std::exchange(gPendingDevice, std::nullopt);
    }
    if (device && gSoundSystem) {
        if (auto const fn = hooks::origSetOutputDeviceFn()) fn(gSoundSystem, device->data());
        logger().info("Export audio capture: replayed deferred setOutputDevice");
    }
    if (enable && gSoundSystem) {
        if (auto const fn = hooks::origEnableFn()) fn(gSoundSystem, *enable);
        logger().info("Export audio capture: replayed deferred enable({})", *enable);
    }
}

void disarmLocked(char const* why) {
    logger().warn("Export audio capture: {}; audio export disabled", why);
    releaseDspLocked();
    restoreOutputLocked();
    gArmed = false;
    hooks::setGateUpdateFloat(false);
}

bool attachDspLocked() {
    void* master = nullptr;
    if (gApi->getMasterChannelGroup(gSystem, &master) != 0 || !master) return false;
    gMaster = master;

    FMOD_DSP_DESCRIPTION desc = {};
    desc.pluginsdkversion     = FMOD_PLUGIN_SDK_VERSION;
    std::snprintf(desc.name, sizeof(desc.name), "PlaybackExportCapture");
    desc.numinputbuffers  = 1;
    desc.numoutputbuffers = 1;
    desc.read             = &captureRead;
    desc.shouldiprocess   = reinterpret_cast<void*>(&captureShouldProcess);

    void* dsp = nullptr;
    if (gApi->createDSP(gSystem, &desc, &dsp) != 0 || !dsp) return false;
    gPhase = int(Phase::Discarding);
    if (gApi->groupAddDSP(master, FMOD_CHANNELCONTROL_DSP_TAIL, dsp) != 0) {
        gApi->dspRelease(dsp);
        return false;
    }
    gDsp       = dsp;
    gCapturing = true;
    int index = -100, count = -1;
    if (gApi->groupGetDSPIndex) gApi->groupGetDSPIndex(master, dsp, &index);
    if (gApi->groupGetNumDSPs) gApi->groupGetNumDSPs(master, &count);
    logger().info("Export audio capture: DSP attached to master TAIL dspIndex={} numDSPs={}", index, count);
    return true;
}

// C(k) = round(k*R*D/N): ideal sample index at the end of accepted frame k.
int64_t idealSamples(int64_t k) { return (k * R * gFrameDen + gFrameNum / 2) / gFrameNum; }

// Move exactly `need` samples into the WAV: drop owed samples first, then copy, pad any shortfall.
void writeSamplesLocked(int64_t need) {
    thread_local std::vector<float> buffer;
    int64_t                         got = 0;
    {
        std::lock_guard plock(gPcmMutex);
        int64_t const avail = int64_t(gPcm.size() / 2);
        int64_t const drop  = std::min(gPadDebt, avail);
        gPadDebt           -= drop;
        got                 = std::min(need, avail - drop);
        buffer.assign(gPcm.begin() + drop * 2, gPcm.begin() + (drop + got) * 2);
        gPcm.erase(gPcm.begin(), gPcm.begin() + (drop + got) * 2);
    }
    for (float v : buffer) {
        float const a = std::fabs(v);
        if (a > gWin.peak) gWin.peak = a;
        gWin.sumSq += double(v) * double(v);
    }
    gWin.sqCount += int64_t(buffer.size());
    if (gWavOk && got > 0) gWav.append(buffer.data(), buffer.size());
    if (got < need) {
        int64_t const missing = need - got;
        if (gWavOk) gWav.padTo(uint64_t(gWritten + need));
        gPadDebt           += missing;
        gWin.paddedSamples += missing;
        gTotalPadded       += missing;
    }
    gWritten += need;
}

bool openWavLocked() {
    if (gSidecar.empty()) return false;
    gWavOk = gWav.open(gSidecar);
    if (!gWavOk) logger().warn("Export audio capture: unable to open sidecar WAV {}", gSidecar.string());
    return gWavOk;
}

void padTailLocked() {
    if (gCapturing && gWavOk) {
        int64_t const target = idealSamples(gAcceptedFrames);
        if (target > gWritten) gWav.padTo(uint64_t(target));
    }
}

} // namespace

bool install() {
    gApi             = &hooks::fmodApi();
    gOrigUpdateFloat = hooks::origUpdateFloatFn();
    gOrigUpdate      = hooks::origUpdateFn();
    if (!gApi->getOutput || !gApi->setOutput || !gApi->getDriver || !gApi->setDriver || !gApi->getMasterChannelGroup
        || !gApi->createDSP || !gApi->groupAddDSP || !gApi->groupStop || !gApi->groupRemoveDSP || !gApi->dspRelease) {
        logger().warn("Export audio capture: required FMOD symbols are unavailable; audio export disabled");
        gApi = nullptr;
        return false;
    }
    hooks::Policy policy;
    // The game's own System::update calls (Ore UI stream helpers) would mix blocks outside the ledger.
    policy.shouldGateUpdate = [] { return gOwnsOutput.load() && !tDriving; };
    policy.onSystemReady    = [](void* system) {
        if (!gArmed.load()) return;
        std::lock_guard lock(gMutex);
        if (system == gSystem) return;
        // A fresh system means the old one (and our DSP) is gone; nothing to restore on it.
        gOwnsOutput = false;
        gDsp        = nullptr;
        gCapturing  = false;
        gArmed      = false;
        hooks::setGateUpdateFloat(false);
        logger().warn("Export audio capture: FMOD system re-initialised during export; audio export disabled");
    };
    policy.deferEnable = [](bool enable) {
        if (!gOwnsOutput.load()) return false;
        std::lock_guard plock(gPendingMutex);
        gPendingEnable = enable;
        return true;
    };
    policy.deferSetOutputDevice = [](void const* deviceId) {
        if (!gOwnsOutput.load() || !deviceId) return false;
        std::array<uint8_t, 16> id{};
        std::memcpy(id.data(), deviceId, id.size());
        std::lock_guard plock(gPendingMutex);
        gPendingDevice = id;
        return true;
    };
    policy.skipStreamPauseWait = [] { return gOwnsOutput.load(); };
    hooks::setPolicy(std::move(policy));
    return true;
}

void uninstall() {
    exportEnd();
    hooks::setPolicy({});
}

void exportBegin(exporting::ExportSettings const& settings) {
    if (!gApi) return;
    std::lock_guard lock(gMutex);
    gFrameNum       = settings.frameRate.numerator > 0 ? settings.frameRate.numerator : 60;
    gFrameDen       = settings.frameRate.denominator > 0 ? settings.frameRate.denominator : 1;
    gSidecar        = settings.outputDirectory / (settings.outputName + ".audio.wav");
    gFirstFrame     = true;
    gAcceptedFrames = 0;
    gWritten        = 0;
    gPadDebt        = 0;
    gLastFrameIndex = 0;
    gWavOk          = false;
    gSavedOutput    = -1;
    gSavedDriver    = -1;
    {
        std::lock_guard plock(gPendingMutex);
        gPendingEnable.reset();
        gPendingDevice.reset();
    }
    resetCounters();
    openWavLocked();
    gArmed = true;
    // Swallow the game's own SoundSystemFMOD::update(float) so replayed sounds stay paused during the seek; the export
    // calls it once per accepted frame instead.
    hooks::setGateUpdateFloat(true);
    gSoundSystem = hooks::currentSoundSystem();
    gSystem      = hooks::currentSystem();
    if (!gSoundSystem || !gSystem) {
        disarmLocked("no live FMOD system");
        gWav.finalize();
        return;
    }

    gApi->getOutput(gSystem, &gSavedOutput);
    gApi->getDriver(gSystem, &gSavedDriver);
    gOwnsOutput      = true; // defer enable/device changes from here on
    int const result = gApi->setOutput(gSystem, FMOD_OUTPUTTYPE_NOSOUND_NRT);
    int       output = -1;
    gApi->getOutput(gSystem, &output);
    if (result != 0 || output != FMOD_OUTPUTTYPE_NOSOUND_NRT) {
        logger().warn("Export audio capture: setOutput(NOSOUND_NRT) result={} output={}", result, output);
        disarmLocked("unable to switch FMOD to non-real-time output");
        gWav.finalize();
        return;
    }
    if (!attachDspLocked()) {
        disarmLocked("unable to attach the capture DSP");
        gWav.finalize();
        return;
    }
    logger().info(
        "Export audio capture: armed (NRT), savedOutput={} savedDriver={} sidecar={} fps={}/{}",
        gSavedOutput,
        gSavedDriver,
        gSidecar.string(),
        gFrameNum,
        gFrameDen
    );
}

void exportEnd() {
    bool const wasArmed = gArmed.exchange(false);
    std::lock_guard lock(gMutex);
    if (wasArmed) {
        logWindowLocked("frames");
        logger().info(
            "Export audio summary: accepted={} written={} expected={} blocks={} mixUpdates={} stalledFrames={} "
            "padded={} chPeak={} peakDb={:.1f}",
            gAcceptedFrames,
            gWritten,
            idealSamples(gAcceptedFrames),
            gBlocks.load(),
            gTotalMixUpdates,
            gTotalStalledFrames,
            gTotalPadded,
            gTotalChannelsPeak,
            toDb(gTotalPeak)
        );
    }
    padTailLocked();
    // Kill the export-timeline sounds so they do not resume on the real output.
    if (gCapturing && gMaster && gApi) gApi->groupStop(gMaster);
    releaseDspLocked();
    if (gApi) restoreOutputLocked();
    hooks::setGateUpdateFloat(false);
    gWav.finalize();
    gSystem      = nullptr;
    gSoundSystem = nullptr;
    gMaster      = nullptr;
    gWavOk       = false;
}

void onFrameAccepted(uint64_t frameIndex) {
    if (!gArmed.load()) return;
    std::lock_guard lock(gMutex);
    if (!gSoundSystem || !gCapturing) return;

    if (gFirstFrame) {
        // seek -> Running boundary: stop everything replayed during the seek, mix and drop the stop ramp, then start
        // the ledger at zero.
        gFirstFrame = false;
        gApi->groupStop(gMaster);
        gPhase = int(Phase::Discarding);
        for (int i = 0; i < FirstFrameFlushUpdates; ++i) mixOneBlockLocked();
        {
            std::lock_guard plock(gPcmMutex);
            gPcm.clear();
        }
        gWritten        = 0;
        gPadDebt        = 0;
        gAcceptedFrames = 0;
        gPhase          = int(Phase::Capturing);
        logger().info("Export audio capture: first frame accepted, seek sounds stopped");
    } else if (frameIndex <= gLastFrameIndex) {
        return; // retry dedup
    }
    gLastFrameIndex = frameIndex;
    ++gAcceptedFrames;

    // One game-layer audio tick per frame: unpause new channels, submit listeners, clean up finished sounds. Its inner
    // System::update mixes the frame's first block.
    auto const t0 = std::chrono::steady_clock::now();
    {
        hooks::UpdateFloatDriveScope drive;
        tDriving = true;
        gOrigUpdateFloat(gSoundSystem, float(gFrameDen) / float(gFrameNum));
        tDriving = false;
    }
    double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++gWin.updates;
    gWin.updMsSum += ms;
    gWin.updMsMax  = std::max(gWin.updMsMax, ms);
    if (gApi->getChannelsPlaying) {
        int playing = 0, real = 0;
        if (gApi->getChannelsPlaying(gSystem, &playing, &real) == 0) {
            gWin.channelsPeak  = std::max(gWin.channelsPeak, playing);
            gTotalChannelsPeak = std::max(gTotalChannelsPeak, playing);
        }
    }

    // Mix until the queue covers this frame (plus any samples owed for earlier padding), then take exactly that.
    int64_t const need     = idealSamples(gAcceptedFrames) - gWritten;
    int const     maxMixes = int(need / 512) + ExtraUpdatesPerFrame;
    int           mixes    = 0;
    while (queuedFrames() - gPadDebt < need && mixes < maxMixes) {
        int64_t const before = gBlocks.load();
        mixOneBlockLocked();
        ++mixes;
        if (gBlocks.load() == before) break; // the DSP did not run: output not started or suspended
    }
    gWin.mixUpdates  += mixes;
    gTotalMixUpdates += mixes;
    if (queuedFrames() - gPadDebt < need) {
        ++gWin.stalledFrames;
        if (gTotalStalledFrames++ == 0)
            logger().warn("Export audio capture: mixer produced no samples at frame {}; padding silence", gAcceptedFrames);
    }
    writeSamplesLocked(need);
    gTotalPeak = std::max(gTotalPeak, gWin.peak);
    if (gAcceptedFrames % SummaryEvery == 0) logWindowLocked("frames");
}

void finalizeSidecar() {
    std::lock_guard lock(gMutex);
    padTailLocked();
    gWav.finalize();
}

bool sessionActive() { return gArmed.load(); }

bool capturing() { return gCapturing; }

std::filesystem::path sidecarPath() {
    std::lock_guard lock(gMutex);
    return gSidecar;
}

} // namespace playback::audio::exportcap
