#include "PreviewAudio.h"

#include "AudioHooks.h"
#include "FmodMinimal.h"

#include "playback/Playback.h"
#include "playback/exporting/ExportActivity.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace playback::audio::preview {

namespace {

using namespace playback::audio::fmod;

std::atomic_bool   gMuted{false};
std::atomic_bool   gClipMuted{false};
std::atomic<float> gVolume{1.0f};
std::atomic_bool   gSilentWhilePaused{true};

// Read by the mixer thread.
std::atomic<float> gGain{1.0f};
std::atomic_bool   gDropNewSounds{false};

// Main thread only.
void* gDspSystem = nullptr;
void* gMaster    = nullptr;
void* gDsp       = nullptr;
bool  gAttachFailed{false};

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

// The gain moves across one block instead of jumping, so muting does not click.
FMOD_RESULT PB_F_CALLBACK
gainRead(FMOD_DSP_STATE*, float* in, float* out, unsigned int length, int inchannels, int* outchannels) {
    static float current = 1.0f;
    *outchannels         = inchannels;
    if (!out || inchannels <= 0 || length == 0) return FMOD_OK;
    float const target = gGain.load(std::memory_order_relaxed);
    if (!in) {
        std::memset(out, 0, size_t(length) * size_t(inchannels) * sizeof(float));
        current = target;
        return FMOD_OK;
    }
    float const step = (target - current) / float(length);
    for (unsigned int i = 0; i < length; ++i) {
        current             += step;
        float const* source  = in + size_t(i) * size_t(inchannels);
        float*       dest    = out + size_t(i) * size_t(inchannels);
        for (int c = 0; c < inchannels; ++c) dest[c] = source[c] * current;
    }
    current = target;
    return FMOD_OK;
}

void attach(CApi const& api, void* system) {
    if (!system || !api.getMasterChannelGroup || !api.createDSP || !api.groupAddDSP || !api.dspRelease) return;
    void* master = nullptr;
    if (api.getMasterChannelGroup(system, &master) != 0 || !master) return;

    FMOD_DSP_DESCRIPTION desc = {};
    desc.pluginsdkversion     = FMOD_PLUGIN_SDK_VERSION;
    std::snprintf(desc.name, sizeof(desc.name), "PlaybackPreviewGain");
    desc.numinputbuffers  = 1;
    desc.numoutputbuffers = 1;
    desc.read             = &gainRead;

    void* dsp = nullptr;
    if (api.createDSP(system, &desc, &dsp) != 0 || !dsp) return;
    // HEAD is the output end of the chain: after the game's own fader, and after the export capture at TAIL.
    if (api.groupAddDSP(master, FMOD_CHANNELCONTROL_DSP_HEAD, dsp) != 0) {
        api.dspRelease(dsp);
        return;
    }
    gDspSystem = system;
    gMaster    = master;
    gDsp       = dsp;
}

void detach(CApi const& api) {
    if (gDsp) {
        if (gMaster && api.groupRemoveDSP) api.groupRemoveDSP(gMaster, gDsp);
        if (api.dspRelease) api.dspRelease(gDsp);
    }
    gDsp       = nullptr;
    gMaster    = nullptr;
    gDspSystem = nullptr;
}

} // namespace

void setMuted(bool muted) { gMuted.store(muted, std::memory_order_relaxed); }

bool isMuted() { return gMuted.load(std::memory_order_relaxed); }

void setClipMuted(bool muted) { gClipMuted.store(muted, std::memory_order_relaxed); }

void setVolume(float volume) { gVolume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed); }

float volume() { return gVolume.load(std::memory_order_relaxed); }

void setSilentWhilePaused(bool silent) { gSilentWhilePaused.store(silent, std::memory_order_relaxed); }

bool isSilentWhilePaused() { return gSilentWhilePaused.load(std::memory_order_relaxed); }

void update(bool replayActive, bool replayPaused, bool replaySeeking, bool exporting) {
    bool const previewing = replayActive && !exporting;
    bool const dropping =
        previewing && (replayPaused || replaySeeking) && gSilentWhilePaused.load(std::memory_order_relaxed);
    gDropNewSounds.store(dropping, std::memory_order_relaxed);
    bool const  muted = gMuted.load(std::memory_order_relaxed) || gClipMuted.load(std::memory_order_relaxed);
    float const gain  = !previewing ? 1.0f : muted ? 0.0f : volume();
    gGain.store(gain, std::memory_order_relaxed);

    // The export owns the mixer while it runs, so the chain is left alone until it is done.
    if (exporting) return;
    auto const& api    = hooks::fmodApi();
    void* const system = hooks::currentSystem();
    if (gDsp && system != gDspSystem) {
        // A rebuilt FMOD system took the old DSP with it.
        gDsp       = nullptr;
        gMaster    = nullptr;
        gDspSystem = nullptr;
    }
    if (gain != 1.0f && !gDsp) {
        if (!system || gAttachFailed) return;
        attach(api, system);
        if (!gDsp) {
            gAttachFailed = true;
            logger().warn("Preview audio: unable to add the gain stage; preview volume and mute are unavailable");
        }
    } else if (gain == 1.0f && gDsp) {
        detach(api);
    }
}

bool shouldDropNewSounds() {
    return gDropNewSounds.load(std::memory_order_relaxed) && !exporting::isExportActivityActive();
}

void reset() {
    gClipMuted.store(false, std::memory_order_relaxed);
    gDropNewSounds.store(false, std::memory_order_relaxed);
    gGain.store(1.0f, std::memory_order_relaxed);
    detach(hooks::fmodApi());
    gAttachFailed = false;
}

} // namespace playback::audio::preview
