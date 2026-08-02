#pragma once

#include <string>

namespace playback::refactor::export_pipeline {

// FFmpeg 构建信息（步骤 1：FfmpegBuildInfo 与构建脚本）
//
// Playback 以 GPL 静态链接方式内置 FFmpeg（见 scripts/build-ffmpeg/）。
// 当 xmake 启用 --playback_ffmpeg=y 且 third_party/ffmpeg 产物存在时，
// 编译期宏 PLAYBACK_HAVE_FFMPEG=1 打开真实 libav 实现；否则返回空值，
// 保证未配置 FFmpeg 的构建仍可编译通过（导出功能被禁用）。
class FfmpegBuildInfo {
public:
    // 静态链接的许可证性质：GPL（启用 libx264/libx265）。
    static constexpr const char* kLicense = "GPL";

    // 例如 "8.1.2"（编译期 FFMPEG_VERSION）。
    static std::string version();

    // configure 参数摘要（libavcodec 编译期配置字符串）。
    static std::string configuration();

    // 各库版本号，例如 "60.26.102"。
    static std::string libavUtilVersion();
    static std::string libavCodecVersion();
    static std::string libavFormatVersion();
    static std::string libswscaleVersion();
    static std::string libswresampleVersion();

    // 运行时探测编码器是否可用（如 "libx264"、"libvpx_vp9"）。
    static bool hasEncoder(const char* encoderName);

    // libav 是否可用（头文件与静态库均已链接且可初始化）。
    static bool isAvailable();
};

} // namespace playback::refactor::export_pipeline
