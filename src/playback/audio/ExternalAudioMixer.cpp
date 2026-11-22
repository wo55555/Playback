#include "ExternalAudioMixer.h"

#include "playback/audio/ExportAudioWav.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace playback::audio::external {

namespace {

std::string pathToUtf8(std::filesystem::path const& path) {
    auto const u8 = path.u8string();
    return std::string(reinterpret_cast<char const*>(u8.data()), u8.size());
}

bool isTrackPlayable(state::editing::model::AudioTrack const& track) noexcept {
    return track.enabled && !track.muted && std::isfinite(track.volume) && track.volume > 0.0f;
}

bool isClipPlayable(state::editing::model::AudioClip const& clip) noexcept {
    return !clip.muted && std::isfinite(clip.volume) && clip.volume > 0.0f && !clip.filePath.empty();
}

double replayTickAtFrame(exporting::CompiledExportPlan const& plan, uint64_t frameIndex) {
    auto const fp = plan.frame(frameIndex);
    if (!fp || fp->replayTickDenominator <= 0) return 0.0;
    return static_cast<double>(fp->replayTickNumerator) / static_cast<double>(fp->replayTickDenominator);
}

bool resolveClipStartInSegment(
    exporting::CompiledExportPlan const&    plan,
    exporting::CompiledExportSegment const& seg,
    int                                     clipStartTick,
    double                                  frameDurSec,
    double&                                 outStartSec
) {
    if (clipStartTick < seg.inTick || clipStartTick >= seg.outTick || seg.frameCount == 0) {
        return false;
    }

    uint64_t const lo         = seg.firstFrame;
    uint64_t const hi         = seg.firstFrame + seg.frameCount;
    double const   targetTick = static_cast<double>(clipStartTick);

    uint64_t left  = lo;
    uint64_t right = hi;
    while (left < right) {
        uint64_t const mid = left + (right - left) / 2;
        if (replayTickAtFrame(plan, mid) < targetTick) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    uint64_t const idx = left;
    if (idx == lo) {
        outStartSec = static_cast<double>(lo) * frameDurSec;
    } else if (idx < hi) {
        double const tPrev = replayTickAtFrame(plan, idx - 1);
        double const tCurr = replayTickAtFrame(plan, idx);
        double const frac  = (tCurr > tPrev) ? std::clamp((targetTick - tPrev) / (tCurr - tPrev), 0.0, 1.0) : 0.0;
        outStartSec        = (static_cast<double>(idx - 1) + frac) * frameDurSec;
    } else {
        double const tLast = replayTickAtFrame(plan, hi - 1);
        double const tEnd  = static_cast<double>(seg.outTick);
        double const frac  = (tEnd > tLast) ? std::clamp((targetTick - tLast) / (tEnd - tLast), 0.0, 1.0) : 0.0;
        outStartSec        = (static_cast<double>(hi - 1) + frac) * frameDurSec;
    }
    return true;
}

bool mixAssetRange(
    DecodedAudioAsset const& asset,
    double                   srcStartSec,
    double                   outStartSec,
    double                   durationSec,
    float                    gain,
    uint16_t                 outChannels,
    size_t                   totalSamples,
    std::vector<float>&      mixBuffer
) {
    if (durationSec <= 0.0 || !std::isfinite(gain) || gain <= 0.0f || asset.frameCount() == 0) return false;

    int64_t const outIdxSigned = std::llround(outStartSec * static_cast<double>(DecodedAudioAsset::SampleRate));
    int64_t const srcIdxSigned = std::llround(srcStartSec * static_cast<double>(DecodedAudioAsset::SampleRate));
    int64_t const countSigned  = std::llround(durationSec * static_cast<double>(DecodedAudioAsset::SampleRate));
    if (countSigned <= 0) return false;

    size_t const outIdx = outIdxSigned > 0 ? static_cast<size_t>(outIdxSigned) : 0;
    size_t const srcIdx = srcIdxSigned > 0 ? static_cast<size_t>(srcIdxSigned) : 0;
    size_t       count  = static_cast<size_t>(countSigned);

    if (outIdx >= totalSamples || srcIdx >= asset.frameCount()) return false;
    count = std::min(count, totalSamples - outIdx);
    count = std::min(count, static_cast<size_t>(asset.frameCount()) - srcIdx);
    if (count == 0) return false;

    for (size_t i = 0; i < count; ++i) {
        float const l = asset.pcmInterleaved[(srcIdx + i) * 2] * gain;
        float const r = asset.pcmInterleaved[(srcIdx + i) * 2 + 1] * gain;
        if (outChannels == 2) {
            mixBuffer[(outIdx + i) * 2]     += l;
            mixBuffer[(outIdx + i) * 2 + 1] += r;
        } else {
            mixBuffer[outIdx + i] += 0.5f * (l + r);
        }
    }
    return true;
}

double computePreStartElapsedSeconds(
    exporting::ExportSettings const& settings,
    int64_t                          firstSegInTick,
    int                              clipStartTick
) noexcept {
    int64_t const deltaTicks = firstSegInTick - static_cast<int64_t>(clipStartTick);
    if (deltaTicks <= 0) return 0.0;
    if (settings.timelapse && settings.timelapseIntervalMs > 0 && settings.frameRate.denominator > 0) {
        double const ticksPerOutputSec =
            (static_cast<double>(settings.timelapseIntervalMs) / 50.0)
            * (static_cast<double>(settings.frameRate.numerator) / static_cast<double>(settings.frameRate.denominator));
        if (ticksPerOutputSec > 0.0) {
            return static_cast<double>(deltaTicks) / ticksPerOutputSec;
        }
    }
    return static_cast<double>(deltaTicks) / 20.0;
}

} // namespace

double effectiveAudioClipDurationSeconds(
    state::editing::model::AudioClip const& clip,
    DecodedAudioAsset const*                asset
) noexcept {
    double const requested = std::isfinite(clip.durationSeconds) ? std::max(0.0, clip.durationSeconds) : 0.0;
    if (!asset || asset->state != AudioLoadState::Ready) {
        return requested;
    }
    double const trimStart = std::isfinite(clip.trimStartSeconds) ? std::max(0.0, clip.trimStartSeconds) : 0.0;
    double const remaining = std::max(0.0, asset->durationSeconds - trimStart);
    if (requested > 0.0) {
        return std::min(requested, remaining);
    }
    return remaining;
}

bool hasPlayableExternalAudio(std::vector<state::editing::model::AudioTrack> const& tracks) noexcept {
    for (auto const& track : tracks) {
        if (!isTrackPlayable(track)) continue;
        for (auto const& clip : track.clips) {
            if (isClipPlayable(clip)) return true;
        }
    }
    return false;
}

bool mixExternalAudioForExport(
    exporting::CompiledExportPlan const&                  plan,
    std::vector<state::editing::model::AudioTrack> const& tracks,
    std::filesystem::path const&                          gameAudioSidecarWav,
    std::filesystem::path const&                          outputWavPath
) {
    if (plan.frameCount == 0 || plan.settings.frameRate.numerator <= 0 || plan.settings.frameRate.denominator <= 0) {
        return false;
    }

    long double const frameDurLd = static_cast<long double>(plan.settings.frameRate.denominator)
                                 / static_cast<long double>(plan.settings.frameRate.numerator);
    long double const totalDurationLd = static_cast<long double>(plan.frameCount) * frameDurLd;
    int64_t const     totalSamplesSigned =
        std::llround(totalDurationLd * static_cast<long double>(DecodedAudioAsset::SampleRate));
    if (totalSamplesSigned <= 0) {
        return false;
    }

    size_t const   totalSamples = static_cast<size_t>(totalSamplesSigned);
    uint16_t const outChannels  = plan.settings.audioStereo ? 2 : 1;
    double const   frameDurSec  = static_cast<double>(frameDurLd);
    double const   totalDurationSec =
        static_cast<double>(totalSamples) / static_cast<double>(DecodedAudioAsset::SampleRate);

    std::vector<float> mixBuffer(totalSamples * outChannels, 0.0f);
    bool               anyAudioMixed = false;

    if (!gameAudioSidecarWav.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(gameAudioSidecarWav, ec) && !ec) {
            auto const gameAsset = loadAudioFileSync(pathToUtf8(gameAudioSidecarWav));
            if (gameAsset && gameAsset->state == AudioLoadState::Ready && gameAsset->frameCount() > 0) {
                if (mixAssetRange(*gameAsset, 0.0, 0.0, totalDurationSec, 1.0f, outChannels, totalSamples, mixBuffer)) {
                    anyAudioMixed = true;
                }
            }
        }
    }

    if (!plan.segments.empty()) {
        auto const& firstSeg = plan.segments.front();
        for (auto const& track : tracks) {
            if (!isTrackPlayable(track)) continue;
            for (auto const& clip : track.clips) {
                if (!isClipPlayable(clip)) continue;

                auto const asset = loadAudioFileSync(clip.filePath);
                if (!asset || asset->state != AudioLoadState::Ready || asset->frameCount() == 0) {
                    continue;
                }

                double const effectiveDur = effectiveAudioClipDurationSeconds(clip, asset.get());
                if (effectiveDur <= 0.0) continue;

                double const trimStart =
                    std::isfinite(clip.trimStartSeconds) ? std::max(0.0, clip.trimStartSeconds) : 0.0;
                float const gain = track.volume * clip.volume;

                if (static_cast<int64_t>(clip.startTick) < firstSeg.inTick) {
                    double const preElapsed =
                        computePreStartElapsedSeconds(plan.settings, firstSeg.inTick, clip.startTick);
                    if (preElapsed < effectiveDur) {
                        double const dur = std::min(effectiveDur - preElapsed, totalDurationSec);
                        if (mixAssetRange(
                                *asset,
                                trimStart + preElapsed,
                                0.0,
                                dur,
                                gain,
                                outChannels,
                                totalSamples,
                                mixBuffer
                            )) {
                            anyAudioMixed = true;
                        }
                    }
                    continue;
                }

                for (auto const& seg : plan.segments) {
                    double outStartSec = 0.0;
                    if (resolveClipStartInSegment(plan, seg, clip.startTick, frameDurSec, outStartSec)) {
                        if (outStartSec < totalDurationSec) {
                            double const dur = std::min(effectiveDur, totalDurationSec - outStartSec);
                            if (mixAssetRange(
                                    *asset,
                                    trimStart,
                                    outStartSec,
                                    dur,
                                    gain,
                                    outChannels,
                                    totalSamples,
                                    mixBuffer
                                )) {
                                anyAudioMixed = true;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    if (!anyAudioMixed) {
        return false;
    }

    for (float& s : mixBuffer) {
        s = std::isfinite(s) ? std::clamp(s, -1.0f, 1.0f) : 0.0f;
    }

    exportcap::WavWriter writer;
    if (!writer.open(outputWavPath, outChannels)) {
        return false;
    }
    writer.append(mixBuffer.data(), mixBuffer.size());
    writer.finalize();
    return true;
}

} // namespace playback::audio::external
