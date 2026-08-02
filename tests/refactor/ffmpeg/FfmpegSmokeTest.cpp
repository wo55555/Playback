// FfmpegSmokeTest — 步骤 1 的链接验证：确认内置静态 FFmpeg 可链接、可初始化。
//
// 运行：xmake f --playback_ffmpeg=y && xmake run refactor-ffmpeg-tests

#include "playback/refactor/export-pipeline/FfmpegBuildInfo.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
using playback::refactor::export_pipeline::FfmpegBuildInfo;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "[FfmpegSmokeTest] FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testBuildInfo() {
    require(FfmpegBuildInfo::isAvailable(), "libav must be available");
    require(!FfmpegBuildInfo::version().empty(), "FFmpeg version must be non-empty");
    require(!FfmpegBuildInfo::configuration().empty(), "configure string must be non-empty");
    require(!FfmpegBuildInfo::libavCodecVersion().empty(), "libavcodec version must be non-empty");
    require(!FfmpegBuildInfo::libavFormatVersion().empty(), "libavformat version must be non-empty");
    require(!FfmpegBuildInfo::libswscaleVersion().empty(), "libswscale version must be non-empty");
    require(!FfmpegBuildInfo::libswresampleVersion().empty(), "libswresample version must be non-empty");
    require(std::string(FfmpegBuildInfo::kLicense) == "GPL", "static link must be GPL");

    require(FfmpegBuildInfo::hasEncoder("libx264"), "libx264 encoder must be enabled");
    require(FfmpegBuildInfo::hasEncoder("libx265"), "libx265 encoder must be enabled");
    require(FfmpegBuildInfo::hasEncoder("libvpx_vp9"), "libvpx_vp9 encoder must be enabled");
    require(FfmpegBuildInfo::hasEncoder("libopus"), "libopus encoder must be enabled");
    require(FfmpegBuildInfo::hasEncoder("aac"), "native aac encoder must be enabled");
}

} // namespace

int main() {
    testBuildInfo();
    std::cout << "[FfmpegSmokeTest] OK  ffmpeg=" << FfmpegBuildInfo::version()
              << "  codec=" << FfmpegBuildInfo::libavCodecVersion()
              << "  gpl=" << FfmpegBuildInfo::hasEncoder("libx264") << '\n';
    return 0;
}
