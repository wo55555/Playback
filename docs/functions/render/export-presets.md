# functions/render/export-presets — 平台 / 分辨率 / 编码器预设

> 入口：`src/playback/functions/render/ExportPresets.{h,cpp}`
> 角色：把 `ExportConfig` 翻译为 **可执行的 FFmpeg 命令参数**，并提供内置的平台预设（YouTube / Bilibili / Twitter / X / IG-Reel / TikTok / PNG 序列）。
> 与 [editor/export-config.md](file:///d:/raplay/Playback/docs/editor/export-config.md) 的关系：消费 `ExportConfig`，产出 `FFmpegCommand`。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| EP-1 | 根据 `ExportConfig` 生成 **完整的 FFmpeg 命令**（输入/输出/编码参数/滤镜） | P0 |
| EP-2 | 自动协商编码器（`h264_nvenc` → `hevc_nvenc` → `h264_qsv` → `h264_amf` → `libx264`） | P0 |
| EP-3 | 支持 6+ 平台预设：YouTube / Bilibili / Twitter(现 X) / IG-Reel / TikTok / PNG | P0 |
| EP-4 | 启动时探测系统可用编码器，缓存到 `EncoderProbeCache` | P0 |
| EP-5 | 提供 "pixel format" 映射（mp4 → yuv420p，ProRes → yuv422p10le） | P0 |
| EP-6 | 透传 `extraFFmpegArgs`（用户自定义） | P0 |
| EP-7 | 支持 H.264/H.265/VP9/ProRes 编码参数默认值 | P0 |
| EP-8 | 提供 **preset → ExportConfig** 的反查（让 UI 能选预设填字段） | P0 |

### 1.2 非功能性需求

- **可读性**：FFmpeg 命令组装顺序固定（输入 → 滤镜 → 编码 → 封装 → 输出），便于排障。
- **可测性**：所有转换函数纯输入输出，无副作用；不启 FFmpeg 进程也能单元测试。
- **可扩展**：新增预设只改 `BuiltinPresets` 数组 + 一个 preset struct。

## 二、架构（Architecture）

### 2.1 内部结构

```
functions/render/
├── ExportPresets.h / .cpp         ← 本模块
├── FFmpegCommand.h                ← 命令结构（可序列化、可调试）
├── EncoderProbe.h / .cpp          ← 编码器探测（一次性）
└── BuiltinPresets.h               ← 6 个内置预设常量表
```

### 2.2 数据模型

```cpp
// FFmpegCommand.h
struct FFmpegCommand {
    std::string                  binary{"ffmpeg"};  // 默认 PATH
    std::vector<std::string>     args;             // 按顺序追加
    std::string                  toDebugString() const;  // 空格拼接 + 引号转义
};

// ExportPresets.h
struct EncoderSpec {
    std::string encoder;       // "libx264" / "h264_nvenc" / "libx265" / "libvpx-vp9" / "prores_ks"
    std::string pixelFormat;   // "yuv420p" / "yuv422p10le"
    std::vector<std::string> defaultArgs;  // ["-preset", "medium", "-tune", "film"]
};

struct PlatformPreset {
    std::string    name;            // 内部 key: "youtube" / "bilibili" ...
    std::string    displayName;     // i18n key
    ExportConfig   config;          // 字段值
    std::string    description;     // i18n key
};

// EncoderProbe.h
struct EncoderProbeResult {
    std::vector<std::string> availableH264;   // ["h264_nvenc", "h264_qsv", "h264_amf", "libx264"]
    std::vector<std::string> availableH265;
    std::vector<std::string> availableVp9;
    bool                     ffmpegAvailable{false};
    std::filesystem::path    ffmpegPath{};
};
```

### 2.3 编码器协商算法

```cpp
// 按优先级返回第一个可用编码器
std::string pickEncoder(VideoCodec requested, const EncoderProbeResult& probe) {
    switch (requested) {
        case VideoCodec::H264: {
            static const std::vector<std::string> kOrder = {
                "h264_nvenc", "h264_qsv", "h264_amf", "libx264"
            };
            for (const auto& e : kOrder) {
                if (std::find(probe.availableH264.begin(), probe.availableH264.end(), e)
                    != probe.availableH264.end()) {
                    return e;
                }
            }
            return "libx264";  // 兜底
        }
        // H.265 / VP9 / ProRes 省略...
    }
}
```

> **降级策略**：硬件编码器不可用时降级到软编（如 `libx264`）。UI 展示"使用的编码器"让用户知情。

### 2.4 编码参数映射

| 用户选项 | FFmpeg args |
|---|---|
| `rcMode = Crf, codec = H264` | `-c:v <enc> -preset medium -crf <crf> -pix_fmt yuv420p` |
| `rcMode = Crf, codec = H265` | `-c:v <enc> -preset medium -crf <crf> -tag:v hvc1 -pix_fmt yuv420p` |
| `rcMode = Crf, codec = VP9` | `-c:v <enc> -b:v 0 -crf <crf> -row-mt 1 -pix_fmt yuv420p` |
| `rcMode = Crf, codec = ProRes` | `-c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le` |
| `rcMode = Cbr, codec = H264` | `-c:v <enc> -preset medium -b:v <bitrate>k -maxrate <bitrate*1.5>k -bufsize <bitrate*2>k -pix_fmt yuv420p` |
| `rcMode = Vbr, codec = H264` | `-c:v <enc> -preset medium -b:v <bitrate>k -pix_fmt yuv420p` |
| `container = Mp4` | `-movflags +faststart -f mp4` |
| `container = Mov` | `-f mov` |
| `container = WebM` | `-f webm` |
| `container = Mkv` | `-f matroska` |
| `container = PngSequence` | 不走 FFmpeg；用 `FrameEncoder::writePngSequence` |
| `audioMode = Include` | `-c:a aac -b:a <audioBitrate>k` |
| `audioMode = Exclude` | `-an` |
| `colorSpace = Hdr10` | `-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020_ncl` |

### 2.5 内置预设

```cpp
// BuiltinPresets.h
inline const std::vector<PlatformPreset> kBuiltin = {
    {
        "youtube", "playback.export.preset.builtin.youtube",
        ExportConfig{
            .aspect = AspectRatio::R16x9, .width = 1920, .height = 1080,
            .fps = 60, .container = ContainerFormat::Mp4, .codec = VideoCodec::H264,
            .rcMode = RateControlMode::Crf, .crf = 18, .bitrateKbps = 12000,
            .audioMode = ExportConfig::AudioMode::Include, .audioBitrateKbps = 192
        },
        "playback.export.preset.builtin.youtube.desc"
    },
    {
        "bilibili", "playback.export.preset.builtin.bilibili",
        ExportConfig{
            .aspect = AspectRatio::R16x9, .width = 1920, .height = 1080,
            .fps = 60, .container = ContainerFormat::Mp4, .codec = VideoCodec::H265,
            .rcMode = RateControlMode::Crf, .crf = 20, .bitrateKbps = 15000,
            .audioMode = ExportConfig::AudioMode::Include, .audioBitrateKbps = 192
        },
        "playback.export.preset.builtin.bilibili.desc"
    },
    {
        "twitter", "playback.export.preset.builtin.twitter",
        ExportConfig{
            .aspect = AspectRatio::R16x9, .width = 1920, .height = 1080,
            .fps = 60, .container = ContainerFormat::Mp4, .codec = VideoCodec::H264,
            .rcMode = RateControlMode::Cbr, .crf = 18, .bitrateKbps = 8000,
            .audioMode = ExportConfig::AudioMode::Include, .audioBitrateKbps = 160
        },
        "playback.export.preset.builtin.twitter.desc"
    },
    {
        "ig-reel", "playback.export.preset.builtin.ig-reel",
        ExportConfig{
            .aspect = AspectRatio::R9x16, .width = 1080, .height = 1920,
            .fps = 30, .container = ContainerFormat::Mp4, .codec = VideoCodec::H264,
            .rcMode = RateControlMode::Cbr, .crf = 20, .bitrateKbps = 6000,
            .audioMode = ExportConfig::AudioMode::Include, .audioBitrateKbps = 128
        },
        "playback.export.preset.builtin.ig-reel.desc"
    },
    {
        "tiktok", "playback.export.preset.builtin.tiktok",
        ExportConfig{
            .aspect = AspectRatio::R9x16, .width = 1080, .height = 1920,
            .fps = 30, .container = ContainerFormat::Mp4, .codec = VideoCodec::H264,
            .rcMode = RateControlMode::Cbr, .crf = 20, .bitrateKbps = 6000,
            .audioMode = ExportConfig::AudioMode::Include, .audioBitrateKbps = 128
        },
        "playback.export.preset.builtin.tiktok.desc"
    },
    {
        "png-sequence", "playback.export.preset.builtin.png-sequence",
        ExportConfig{
            .aspect = AspectRatio::R16x9, .width = 1920, .height = 1080,
            .fps = 60, .container = ContainerFormat::PngSequence, .codec = VideoCodec::PngSequence,
            .rcMode = RateControlMode::Crf, .crf = 0, .bitrateKbps = 0,
            .audioMode = ExportConfig::AudioMode::Exclude
        },
        "playback.export.preset.builtin.png-sequence.desc"
    }
};
```

### 2.6 命令组装流程

```cpp
FFmpegCommand ExportPresets::buildCommand(
    const ExportConfig& cfg,
    const EncoderProbeResult& probe,
    const std::string& rawVideoPipePath,  // e.g. \\.\pipe\playback_video
    const std::string& rawAudioPipePath   // e.g. \\.\pipe\playback_audio (optional)
) {
    FFmpegCommand cmd;
    if (!probe.ffmpegPath.empty()) {
        cmd.binary = probe.ffmpegPath.string();
    }

    // 1. 全局参数
    cmd.args.push_back("-y");                          // 覆盖输出
    cmd.args.push_back("-hide_banner");
    cmd.args.push_back("-loglevel"); cmd.args.push_back("error");
    cmd.args.push_back("-fflags"); cmd.args.push_back("+genpts");
    cmd.args.push_back("-f"); cmd.args.push_back("rawvideo");
    cmd.args.push_back("-pix_fmt"); cmd.args.push_back("bgra");
    cmd.args.push_back("-s"); cmd.args.push_back(std::format("{}x{}", cfg.width, cfg.height));
    cmd.args.push_back("-r"); cmd.args.push_back(std::to_string(cfg.fps));
    cmd.args.push_back("-i"); cmd.args.push_back(rawVideoPipePath);

    // 2. 音频输入
    if (cfg.audioMode == ExportConfig::AudioMode::Include ||
        cfg.audioMode == ExportConfig::AudioMode::OnlyAudio) {
        cmd.args.push_back("-f"); cmd.args.push_back("s16le");
        cmd.args.push_back("-ar"); cmd.args.push_back("48000");
        cmd.args.push_back("-ac"); cmd.args.push_back("2");
        cmd.args.push_back("-i"); cmd.args.push_back(rawAudioPipePath);
    }

    // 3. 视频编码参数
    if (cfg.container != ContainerFormat::PngSequence) {
        std::string enc = pickEncoder(cfg.codec, probe);
        cmd.args.push_back("-c:v"); cmd.args.push_back(enc);
        appendRateControl(cmd, cfg);
        cmd.args.push_back("-pix_fmt"); cmd.args.push_back(pixelFormatFor(cfg.codec));
    }

    // 4. 音频编码
    if (cfg.audioMode == ExportConfig::AudioMode::Include) {
        cmd.args.push_back("-c:a"); cmd.args.push_back("aac");
        cmd.args.push_back("-b:a"); cmd.args.push_back(std::format("{}k", cfg.audioBitrateKbps));
    } else if (cfg.audioMode == ExportConfig::AudioMode::Exclude) {
        cmd.args.push_back("-an");
    }

    // 5. 颜色空间
    if (cfg.colorSpace == ExportConfig::ColorSpace::Hdr10) {
        cmd.args.push_back("-color_primaries"); cmd.args.push_back("bt2020");
        cmd.args.push_back("-color_trc"); cmd.args.push_back("smpte2084");
        cmd.args.push_back("-colorspace"); cmd.args.push_back("bt2020_ncl");
    }

    // 6. 容器 / 输出
    if (cfg.container == ContainerFormat::Mp4) {
        cmd.args.push_back("-movflags"); cmd.args.push_back("+faststart");
        cmd.args.push_back("-f"); cmd.args.push_back("mp4");
    } else if (cfg.container == ContainerFormat::WebM) {
        cmd.args.push_back("-f"); cmd.args.push_back("webm");
    } else if (cfg.container == ContainerFormat::Mkv) {
        cmd.args.push_back("-f"); cmd.args.push_back("matroska");
    } else if (cfg.container == ContainerFormat::Mov) {
        cmd.args.push_back("-f"); cmd.args.push_back("mov");
    }

    // 7. 用户透传
    if (!cfg.extraFFmpegArgs.empty()) {
        auto extras = splitArgs(cfg.extraFFmpegArgs);
        for (auto& e : extras) cmd.args.push_back(std::move(e));
    }

    // 8. 输出路径
    cmd.args.push_back(cfg.outputPath.string());

    return cmd;
}
```

### 2.7 编码器探测

```cpp
// EncoderProbe.cpp
EncoderProbeResult probeEncoders(const std::filesystem::path& ffmpegBinary) {
    EncoderProbeResult r;
    r.ffmpegPath = findFFmpeg();  // 1) PATH 2) 同目录 3) 注册表

    if (r.ffmpegPath.empty()) {
        return r;  // 不可用
    }
    r.ffmpegAvailable = true;

    // ffmpeg -hide_banner -encoders
    auto output = runProcess(r.ffmpegPath, {"-hide_banner", "-encoders"});
    if (output.find("h264_nvenc") != std::string::npos) r.availableH264.push_back("h264_nvenc");
    if (output.find("h264_qsv")   != std::string::npos) r.availableH264.push_back("h264_qsv");
    if (output.find("h264_amf")   != std::string::npos) r.availableH264.push_back("h264_amf");
    if (output.find("libx264")    != std::string::npos) r.availableH264.push_back("libx264");
    // H.265 / VP9 同理...

    return r;
}
```

**缓存策略**：探测结果存 `data/exports/encoder-probe.json`，key = ffmpeg binary 路径 + 修改时间。命中缓存不重复探测。

## 三、执行（Execution）

### 3.1 任务拆分

| 步骤 | 文件 | 验证 |
|---|---|---|
| 1 | `FFmpegCommand.h` | 编译 + toDebugString 正确 |
| 2 | `EncoderProbe.{h,cpp}` | 手动：探测真实 ffmpeg |
| 3 | `BuiltinPresets.h` | 单测：6 个 preset 字段都填对 |
| 4 | `ExportPresets.{h,cpp}` 主算法 | 单测：每种 codec/rc 组合 args 正确 |
| 5 | 探测结果缓存 | 手动：第二次启动不重复探测 |
| 6 | 单元测试：每种容器+编码器组合 | 全部通过 |

### 3.2 关键不变量

1. **命令可重现**：相同 `ExportConfig` + 相同 `probe` → 相同 `FFmpegCommand`（无随机种子）。
2. **编码器列表有序**：协商时按 `[NVENC, QSV, AMF, libx264]` 优先级，第一个可用胜出。
3. **预设版本化**：修改内置预设时 `version++`；UI 加载旧版本预设时 warn。
4. **PNG 序列不调 FFmpeg**：`container == PngSequence` 时 `buildCommand` 返回空 `args`，由 `FrameEncoder` 直接写 PNG。

### 3.3 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| EP-T1 | H.264 + CRF + MP4 | args 含 `-c:v libx264 -crf 18 -pix_fmt yuv420p -movflags +faststart` |
| EP-T2 | H.265 + CBR + MKV | args 含 `-c:v libx265 -b:v 15000k` |
| EP-T3 | WebM + VP9 | args 含 `-c:v libvpx-vp9 -b:v 0` |
| EP-T4 | PngSequence | args 空，触发 `writePngSequence` 分支 |
| EP-T5 | 探测缓存命中 | 第二次 `probeEncoders` 不启 ffmpeg 进程 |
| EP-T6 | YouTube 预设反查 | 字段完全匹配 1920x1080/60fps/CRF 18 |

### 3.4 风险与回退

| 风险 | 缓解 |
|---|---|
| FFmpeg 不在 PATH | 模组同目录放 `ffmpeg.exe`；提供"下载 FFmpeg"按钮（curl 内置） |
| 硬编 SDK 失败 | 协商降级到 libx264；UI 提示"已切换到软编" |
| 旧 FFmpeg 不支持新参数 | 探测时 `ffmpeg -version` 解析 major version；新参数被拒时 warn |
| 平台预设违规 | YouTube 推荐 60fps H.264，但用户可改；不改不报错 |

## 四、模块关系

### 被谁调用（上游）

- **`functions/render/FrameEncoder`**：拿 `FFmpegCommand` 启子进程。
- **`functions/render/RenderJob`**：启动时调 `probeEncoders` + `buildCommand`。
- **`editor/ui/panels/PresetSelector`**：反查 `BuiltinPresets` 填字段。

### 调用谁（下游）

- **`std::process` / Win32 `CreateProcessW`**：启 FFmpeg。
- **`nlohmann::json`**：缓存探测结果。

### 共享数据

- 全局 `gEncoderProbe`：单例，第一次 `probeEncoders` 后缓存。

### 事件订阅 / 发送

- 无。

## 五、阅读顺序

1. 本文件
2. [editor/export-config.md](file:///d:/raplay/Playback/docs/editor/export-config.md)
3. [functions/render/frame-encoder.md](file:///d:/raplay/Playback/docs/functions/render/frame-encoder.md)
