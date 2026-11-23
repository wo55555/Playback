#include "ExternalAudioPreview.h"

#include "playback/audio/ExternalAudioCache.h"
#include "playback/audio/ExternalAudioMixer.h"
#include "playback/keyframe/SpeedTimelineEvaluator.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace playback::audio::external {

namespace {

constexpr DWORD  kSampleRate             = 48000;
constexpr WORD   kChannels               = 2;
constexpr WORD   kBitsPerSample          = 16;
constexpr size_t kBufferCount            = 4;
constexpr size_t kFramesPerBuffer        = 960; // 20 ms at 48 kHz
constexpr size_t kSamplesPerBuffer       = kFramesPerBuffer * kChannels;
constexpr DWORD  kBytesPerBuffer         = static_cast<DWORD>(kSamplesPerBuffer * sizeof(int16_t));
constexpr double kBufferDurationSec      = static_cast<double>(kFramesPerBuffer) / static_cast<double>(kSampleRate);
constexpr double kClockDriftThresholdSec = 0.15;

bool sameSpeedTrack(state::editing::model::SpeedTrack const& a, state::editing::model::SpeedTrack const& b) noexcept {
    if (a.present != b.present || a.enabled != b.enabled || a.locked != b.locked
        || a.keysByTick.size() != b.keysByTick.size()) {
        return false;
    }
    auto itA = a.keysByTick.begin();
    auto itB = b.keysByTick.begin();
    for (; itA != a.keysByTick.end(); ++itA, ++itB) {
        if (itA->first != itB->first) return false;
        auto const& ka = itA->second;
        auto const& kb = itB->second;
        if (ka.speed != kb.speed || ka.interpolationType != kb.interpolationType || ka.bezierCtrl1.x != kb.bezierCtrl1.x
            || ka.bezierCtrl1.y != kb.bezierCtrl1.y || ka.bezierCtrl2.x != kb.bezierCtrl2.x
            || ka.bezierCtrl2.y != kb.bezierCtrl2.y) {
            return false;
        }
    }
    return true;
}

class ExternalPreviewEngine {
public:
    ~ExternalPreviewEngine() { shutdown(); }

    void update(
        state::editing::model::EditorStateExt const& project,
        long double                                  replayTick,
        bool                                         playing,
        bool                                         seeking,
        bool                                         exporting,
        float                                        masterVolume,
        bool                                         masterMuted
    ) {
        if (!playing || seeking || exporting || masterMuted || !hasPlayableExternalAudio(project.audioTracks)) {
            stop();
            return;
        }

        std::lock_guard lock(mMutex);

        if (!ensureMapping(project)) {
            return;
        }
        if (!ensureDeviceOpen()) {
            return;
        }

        reclaimCompletedSlots();

        double const nowOutSec     = mMapping->unscaledTicksAt(replayTick) / 20.0;
        size_t const inFlightCount = countInFlightSlots();
        double const approxPlaySec = mNextWriteOutSec - static_cast<double>(inFlightCount) * kBufferDurationSec;

        if (!mStreamAnchored || std::abs(nowOutSec - approxPlaySec) > kClockDriftThresholdSec) {
            resetStreamLocked();
            mNextWriteOutSec = nowOutSec;
            mStreamAnchored  = true;
        } else if (inFlightCount == 0 && nowOutSec > mNextWriteOutSec) {
            mNextWriteOutSec = nowOutSec;
        }

        float const safeMasterVol = std::isfinite(masterVolume) ? std::clamp(masterVolume, 0.0f, 2.0f) : 1.0f;

        for (auto& slot : mSlots) {
            if (slot.inFlight || !slot.prepared) continue;
            fillBufferLocked(project, mNextWriteOutSec, safeMasterVol, slot.pcm);
            slot.header.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
            if (waveOutWrite(mWaveOut, &slot.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                closeDeviceLocked();
                return;
            }
            slot.inFlight     = true;
            mNextWriteOutSec += kBufferDurationSec;
        }
    }

    void stop() {
        std::lock_guard lock(mMutex);
        if (mWaveOut != nullptr && (mStreamAnchored || countInFlightSlots() > 0)) {
            resetStreamLocked();
        }
        mStreamAnchored  = false;
        mNextWriteOutSec = 0.0;
    }

    void shutdown() {
        std::lock_guard lock(mMutex);
        closeDeviceLocked();
        mMapping.reset();
        mCachedSpeedTrack     = {};
        mCachedTimelapseTrack = {};
        mCachedTotalTicks     = -1;
        mStreamAnchored       = false;
        mNextWriteOutSec      = 0.0;
    }

private:
    struct WaveBufferSlot {
        WAVEHDR              header{};
        std::vector<int16_t> pcm;
        bool                 prepared{false};
        bool                 inFlight{false};
    };

    bool ensureMapping(state::editing::model::EditorStateExt const& project) {
        if (!mMapping || mCachedTotalTicks != project.totalTicks
            || !sameSpeedTrack(mCachedSpeedTrack, project.speedTrack)
            || !(mCachedTimelapseTrack == project.timelapseTrack)) {
            mCachedSpeedTrack     = project.speedTrack;
            mCachedTimelapseTrack = project.timelapseTrack;
            mCachedTotalTicks     = project.totalTicks;
            mMapping.emplace(project);
        }
        return mMapping.has_value();
    }

    bool ensureDeviceOpen() {
        if (mWaveOut != nullptr) return true;

        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = kChannels;
        wfx.nSamplesPerSec  = kSampleRate;
        wfx.wBitsPerSample  = kBitsPerSample;
        wfx.nBlockAlign     = kChannels * (kBitsPerSample / 8);
        wfx.nAvgBytesPerSec = kSampleRate * wfx.nBlockAlign;
        wfx.cbSize          = 0;

        if (waveOutOpen(&mWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            mWaveOut = nullptr;
            return false;
        }

        for (auto& slot : mSlots) {
            slot.pcm.assign(kSamplesPerBuffer, 0);
            slot.header                = {};
            slot.header.lpData         = reinterpret_cast<LPSTR>(slot.pcm.data());
            slot.header.dwBufferLength = kBytesPerBuffer;
            slot.inFlight              = false;
            if (waveOutPrepareHeader(mWaveOut, &slot.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                closeDeviceLocked();
                return false;
            }
            slot.prepared = true;
        }
        return true;
    }

    void closeDeviceLocked() {
        if (mWaveOut == nullptr) return;
        waveOutReset(mWaveOut);
        for (auto& slot : mSlots) {
            if (slot.prepared) {
                waveOutUnprepareHeader(mWaveOut, &slot.header, sizeof(WAVEHDR));
                slot.prepared = false;
            }
            slot.inFlight = false;
            slot.header   = {};
            slot.pcm.clear();
        }
        waveOutClose(mWaveOut);
        mWaveOut        = nullptr;
        mStreamAnchored = false;
    }

    void resetStreamLocked() {
        if (mWaveOut == nullptr) return;
        waveOutReset(mWaveOut);
        for (auto& slot : mSlots) {
            slot.inFlight = false;
        }
    }

    void reclaimCompletedSlots() {
        for (auto& slot : mSlots) {
            if (slot.inFlight && (slot.header.dwFlags & WHDR_DONE) != 0) {
                slot.inFlight = false;
            }
        }
    }

    [[nodiscard]] size_t countInFlightSlots() const noexcept {
        size_t count = 0;
        for (auto const& slot : mSlots) {
            if (slot.inFlight) ++count;
        }
        return count;
    }

    void fillBufferLocked(
        state::editing::model::EditorStateExt const& project,
        double                                       bufStartOutSec,
        float                                        masterVolume,
        std::vector<int16_t>&                        outPcm
    ) {
        std::array<float, kSamplesPerBuffer> mixFloat{};
        double const                         bufEndOutSec = bufStartOutSec + kBufferDurationSec;

        for (auto const& track : project.audioTracks) {
            if (!track.enabled || track.muted || !std::isfinite(track.volume) || track.volume <= 0.0f) continue;
            for (auto const& clip : track.clips) {
                if (clip.muted || !std::isfinite(clip.volume) || clip.volume <= 0.0f || clip.filePath.empty()) {
                    continue;
                }

                auto const asset = getOrRequestAudioAsset(clip.filePath);
                if (!asset || asset->state != AudioLoadState::Ready || asset->frameCount() == 0) {
                    continue;
                }

                double const effectiveDur = effectiveAudioClipDurationSeconds(clip, asset.get());
                if (effectiveDur <= 0.0) continue;

                double const clipStartOutSec =
                    mMapping->unscaledTicksAt(static_cast<long double>(clip.startTick)) / 20.0;
                double const clipEndOutSec = clipStartOutSec + effectiveDur;
                if (clipEndOutSec <= bufStartOutSec || clipStartOutSec >= bufEndOutSec) {
                    continue;
                }

                double const trimStart =
                    std::isfinite(clip.trimStartSeconds) ? std::max(0.0, clip.trimStartSeconds) : 0.0;
                float const gain = track.volume * clip.volume * masterVolume;
                if (!std::isfinite(gain) || gain <= 0.0f) continue;

                for (size_t f = 0; f < kFramesPerBuffer; ++f) {
                    double const sampleOutSec =
                        bufStartOutSec + (static_cast<double>(f) + 0.5) / static_cast<double>(kSampleRate);
                    double const tInClip = sampleOutSec - clipStartOutSec;
                    if (tInClip < 0.0 || tInClip >= effectiveDur) continue;

                    int64_t const srcIdxSigned =
                        std::llround((trimStart + tInClip) * static_cast<double>(DecodedAudioAsset::SampleRate));
                    if (srcIdxSigned < 0) continue;
                    size_t const srcIdx = static_cast<size_t>(srcIdxSigned);
                    if (srcIdx >= asset->frameCount()) continue;

                    mixFloat[f * 2]     += asset->pcmInterleaved[srcIdx * 2] * gain;
                    mixFloat[f * 2 + 1] += asset->pcmInterleaved[srcIdx * 2 + 1] * gain;
                }
            }
        }

        for (size_t i = 0; i < kSamplesPerBuffer; ++i) {
            float const clamped = std::isfinite(mixFloat[i]) ? std::clamp(mixFloat[i], -1.0f, 1.0f) : 0.0f;
            long const  scaled  = std::lround(clamped * 32767.0f);
            outPcm[i]           = static_cast<int16_t>(std::clamp(scaled, -32767L, 32767L));
        }
    }

    std::mutex                                mMutex;
    HWAVEOUT                                  mWaveOut{nullptr};
    std::array<WaveBufferSlot, kBufferCount>  mSlots;
    bool                                      mStreamAnchored{false};
    double                                    mNextWriteOutSec{0.0};
    std::optional<keyframe::SpeedTimeMapping> mMapping;
    state::editing::model::SpeedTrack         mCachedSpeedTrack;
    state::editing::model::TimelapseTrack     mCachedTimelapseTrack;
    int                                       mCachedTotalTicks{-1};
};

ExternalPreviewEngine& previewEngine() {
    static ExternalPreviewEngine instance;
    return instance;
}

} // namespace

void updatePreview(
    state::editing::model::EditorStateExt const& project,
    long double                                  replayTick,
    bool                                         playing,
    bool                                         seeking,
    bool                                         exporting,
    float                                        masterVolume,
    bool                                         masterMuted
) {
    previewEngine().update(project, replayTick, playing, seeking, exporting, masterVolume, masterMuted);
}

void stopPreview() { previewEngine().stop(); }

void shutdownPreview() { previewEngine().shutdown(); }

} // namespace playback::audio::external
