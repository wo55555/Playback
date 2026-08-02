#include "FfmpegBuildInfo.h"

#ifdef PLAYBACK_HAVE_FFMPEG

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

namespace playback::refactor::export_pipeline {

std::string FfmpegBuildInfo::version() {
    const char* v = av_version_info();
    return v ? v : "";
}

std::string FfmpegBuildInfo::configuration() {
    const char* c = avcodec_configuration();
    return c ? c : "";
}

std::string FfmpegBuildInfo::libavUtilVersion() {
    return av_version_info() ? std::to_string(LIBAVUTIL_VERSION_MAJOR) + "." +
                                   std::to_string(LIBAVUTIL_VERSION_MINOR) + "." +
                                   std::to_string(LIBAVUTIL_VERSION_MICRO)
                             : "";
}

std::string FfmpegBuildInfo::libavCodecVersion() {
    return av_version_info() ? std::to_string(LIBAVCODEC_VERSION_MAJOR) + "." +
                                   std::to_string(LIBAVCODEC_VERSION_MINOR) + "." +
                                   std::to_string(LIBAVCODEC_VERSION_MICRO)
                             : "";
}

std::string FfmpegBuildInfo::libavFormatVersion() {
    return av_version_info() ? std::to_string(LIBAVFORMAT_VERSION_MAJOR) + "." +
                                   std::to_string(LIBAVFORMAT_VERSION_MINOR) + "." +
                                   std::to_string(LIBAVFORMAT_VERSION_MICRO)
                             : "";
}

std::string FfmpegBuildInfo::libswscaleVersion() {
    return av_version_info() ? std::to_string(LIBSWSCALE_VERSION_MAJOR) + "." +
                                   std::to_string(LIBSWSCALE_VERSION_MINOR) + "." +
                                   std::to_string(LIBSWSCALE_VERSION_MICRO)
                             : "";
}

std::string FfmpegBuildInfo::libswresampleVersion() {
    return av_version_info() ? std::to_string(LIBSWRESAMPLE_VERSION_MAJOR) + "." +
                                   std::to_string(LIBSWRESAMPLE_VERSION_MINOR) + "." +
                                   std::to_string(LIBSWRESAMPLE_VERSION_MICRO)
                             : "";
}

bool FfmpegBuildInfo::hasEncoder(const char* encoderName) {
    if (!encoderName) {
        return false;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name(encoderName);
    return codec != nullptr;
}

bool FfmpegBuildInfo::isAvailable() {
    return av_version_info() != nullptr;
}

} // namespace playback::refactor::export_pipeline

#else // !PLAYBACK_HAVE_FFMPEG

namespace playback::refactor::export_pipeline {

std::string FfmpegBuildInfo::version() { return ""; }
std::string FfmpegBuildInfo::configuration() { return ""; }
std::string FfmpegBuildInfo::libavUtilVersion() { return ""; }
std::string FfmpegBuildInfo::libavCodecVersion() { return ""; }
std::string FfmpegBuildInfo::libavFormatVersion() { return ""; }
std::string FfmpegBuildInfo::libswscaleVersion() { return ""; }
std::string FfmpegBuildInfo::libswresampleVersion() { return ""; }

bool FfmpegBuildInfo::hasEncoder(const char*) { return false; }
bool FfmpegBuildInfo::isAvailable() { return false; }

} // namespace playback::refactor::export_pipeline

#endif // PLAYBACK_HAVE_FFMPEG
