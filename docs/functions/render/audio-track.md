# functions/render/audio-track — 音频轨道（重采样 + 混音 + 编码）

> 入口：`src/playback/functions/render/AudioTrack.{h,cpp}`
> 角色：从 `.playback` 的客户端安全包中重建 **音频源流**（玩家位置音频、环境音、物品音效），按导出 FPS / 时长 **重采样到 48kHz 立体声 PCM**，喂给 `FrameEncoder` 的音频 pipe。
> 当前 Playback 录制层**未捕获音频**（见 [functions/record.md](file:///d:/raplay/Playback/docs/functions/record.md) `recordGamePacket` 过滤列表）；本模块先行 **预留接口**，等 `Recorder` 增音频抓包后启用。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| AT-1 | 从 `ReplaySession` 拿"原始音频源"（未来扩展点，本期 stub） | P2 |
| AT-2 | 内部 **48kHz 立体声 PCM** 统一格式 | P0 |
| AT-3 | `feedTick(tick, dt)` 把 tick 范围的音频拉进内部 ring buffer | P0 |
| AT-4 | `drainFrames(frameCount)` 拿出 `frameCount` 个立体声采样 | P0 |
| AT-5 | 提供"静音生成"（`audioMode == Include` 但无源时） | P0 |
| AT-6 | 提供"旁路"（`audioMode == Exclude`）—— `drainFrames` 返 0 帧 | P0 |
| AT-7 | 输出格式 `s16le` 立体声 48kHz，对齐 `ExportPresets` 的 FFmpeg 输入 | P0 |

### 1.2 非功能性需求

- **延迟**：feed / drain 每次 < 1ms。
- **同步**：每帧音频量 = `1/fps` 秒（48000 / fps = 800 帧 @60fps）。
- **内存**：ring buffer 容量 = 1 秒 × 4 字节 × 2 通道 = 384KB 足够。
- **线程**：仅 RenderJob worker 调用；无锁。

### 1.3 与现有约束对齐

- 复用 `ReplaySession` 的 `getCurrentTick()` 推进信号。
- 复用 `FrameEncoder` 的 `writeAudioChunk(int16_t*, frames)` 写入路径。

## 二、架构（Architecture）

### 2.1 内部结构

```
functions/render/
├── AudioTrack.h / .cpp            ← 本模块
├── AudioRingBuffer.h              ← 内部环形缓冲
├── AudioResampler.h               ← 重采样（首期固定 48kHz，留接口）
└── AudioSourceStub.h              ← 占位：未来接 .playback 音频
```

### 2.2 主类

```cpp
// AudioTrack.h
class AudioTrack {
public:
    enum class Mode { Include, Exclude, OnlyAudio };

    void initialize(Mode mode, int targetSampleRate = 48000);
    void shutdown();

    // RenderJob 调用
    void feedTick(int tick, float dtSec);                 // 把 tick 范围的源音频写入 ring
    size_t drainFrames(int16_t* dst, size_t maxFrames);   // 拿出 N 帧 PCM

    // 统计
    size_t totalFramesWritten() const { return mTotalFrames; }
    bool   isEnabled() const { return mMode != Mode::Exclude; }

private:
    Mode       mMode{Mode::Exclude};
    int        mSampleRate{48000};
    int        mChannels{2};
    AudioRingBuffer mRing;
    size_t     mTotalFrames{0};

    // 未来扩展
    std::unique_ptr<AudioSourceStub> mSource;
};
```

### 2.3 环形缓冲

```cpp
// AudioRingBuffer.h
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacityFrames)
        : mBuf(capacityFrames * 2) {}  // 立体声

    void push(const int16_t* src, size_t frames) {
        for (size_t i = 0; i < frames * 2; ++i) {
            mBuf[mHead++ % mBuf.size()] = src[i];
        }
    }
    size_t pop(int16_t* dst, size_t frames) {
        size_t n = std::min(frames, available());
        for (size_t i = 0; i < n * 2; ++i) {
            dst[i] = mBuf[mTail++ % mBuf.size()];
        }
        return n;
    }
    size_t available() const {
        return (mHead - mTail) / 2;
    }
    void clear() { mHead = mTail = 0; }

private:
    std::vector<int16_t> mBuf;
    size_t mHead{0}, mTail{0};
};
```

### 2.4 每帧喂数据

```cpp
void AudioTrack::feedTick(int tick, float dtSec) {
    if (mMode == Mode::Exclude) return;

    // 1) 拿源音频（本版：静音生成）
    size_t frames = static_cast<size_t>(dtSec * mSampleRate);
    std::vector<int16_t> src(frames * mChannels, 0);

    if (mSource) {
        mSource->pullForTick(tick, dtSec, src.data(), frames);
    }

    // 2) 重采样到 48kHz（本版：直通，src 已是 48kHz）
    // 未来若源采样率不同：mResampler->process(src, dst)

    // 3) 入 ring
    mRing.push(src.data(), frames);
}
```

### 2.5 每帧取数据

```cpp
size_t AudioTrack::drainFrames(int16_t* dst, size_t maxFrames) {
    if (mMode == Mode::Exclude) return 0;

    size_t got = mRing.pop(dst, maxFrames);

    // 不足补静音
    if (got < maxFrames) {
        std::memset(dst + got * mChannels, 0, (maxFrames - got) * mChannels * sizeof(int16_t));
    }
    mTotalFrames += maxFrames;
    return maxFrames;  // 永远返 maxFrames（保证 PTS 稳定）
}
```

### 2.6 未来扩展：接 `.playback` 音频

`.playback` ZIP 增 `audio/` 目录：每 tick 一个 PCM 文件（与视频帧对齐）。

```cpp
class PlaybackFileAudioSource : public AudioSourceStub {
    std::unordered_map<int, std::vector<int16_t>> mTickCache;  // tick -> PCM
    ReplayReader mReader;

    void pullForTick(int tick, float dtSec, int16_t* dst, size_t frames) override {
        if (mTickCache.contains(tick)) {
            std::memcpy(dst, mTickCache[tick].data(), std::min(frames, mTickCache[tick].size()/2) * 4);
        }
    }
};
```

> **本期**：AudioSourceStub 永远返回静音；FrameEncoder 仍按 `Include` 模式生成带音轨的 mp4（静音轨）。

### 2.7 与 FrameEncoder 的集成

```cpp
// RenderJob 主循环内
void RenderJob::writeAudioChunkIfNeeded(int frameIdx) {
    if (!mAudioTrack || !mAudioTrack->isEnabled()) return;

    size_t framesPerVideoFrame = mConfig.fps ? (mAudioTrack->sampleRate() / mConfig.fps) : 0;
    if (framesPerVideoFrame == 0) return;

    int16_t buf[4096 * 2];
    if (framesPerVideoFrame > 4096) {
        // 分块（极端 fps=12 时 48000/12=4000）
        std::vector<int16_t> big(framesPerVideoFrame * 2);
        mAudioTrack->drainFrames(big.data(), framesPerVideoFrame);
        mFrameEncoder->writeAudioChunk(big.data(), framesPerVideoFrame);
    } else {
        mAudioTrack->drainFrames(buf, framesPerVideoFrame);
        mFrameEncoder->writeAudioChunk(buf, framesPerVideoFrame);
    }
}
```

## 三、执行（Execution）

### 3.1 任务拆分

| 步骤 | 文件 | 验证 |
|---|---|---|
| 1 | `AudioRingBuffer.{h,cpp}` | 单测：push/pop 边界 |
| 2 | `AudioTrack` 主类 + Exclude/Include/OnlyAudio 模式 | 单测：drainFrames 返 0 或静音 |
| 3 | 静音生成 | 手动：导出 mp4 在 VLC 播，音轨存在但无声 |
| 4 | `writeAudioChunk` 联动 FrameEncoder | 手动：ffprobe 看 mp4 音轨参数 |
| 5 | AudioSourceStub 接口 | 编译 |
| 6 | (未来) PlaybackFileAudioSource | 等 Recorder 增音频抓包 |

### 3.2 关键算法

**每视频帧音频量**：

```cpp
size_t framesPerVideoFrame(int fps, int sampleRate) {
    return static_cast<size_t>(sampleRate / fps);
}
```

**不足补静音**：

```cpp
if (got < maxFrames) {
    std::memset(dst + got * mChannels, 0, (maxFrames - got) * mChannels * sizeof(int16_t));
}
```

### 3.3 关键不变量

1. **drainFrames 永远返 maxFrames**：保证音频 PTS 与视频帧对齐；不足补静音。
2. **feedTick 早于 drainFrames**：每个视频帧先 feed 再 drain（同一 tick 范围内）。
3. **Exclude 模式不分配资源**：`mRing` 不创建。
4. **静音源是合法输出**：`Include` 模式 + 无源 = 静音 mp4，**不是**错误。

### 3.4 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| AT-T1 | Exclude 模式 drain | 返 0 |
| AT-T2 | Include 模式无源 drain | 返 maxFrames，值全 0 |
| AT-T3 | Ring buffer 边界 | push 满 + pop + push 不丢 |
| AT-T4 | FPS=60, sampleRate=48000 | framesPerVideoFrame=800 |
| AT-T5 | FPS=24 | framesPerVideoFrame=2000 |
| AT-T6 | OnlyAudio 模式 | 行为同 Include |

### 3.5 风险与回退

| 风险 | 缓解 |
|---|---|
| Ring buffer 满（drain 慢） | capacity = 2 秒（防 1s 抖动） |
| 源采样率非 48kHz | 本期固定 48kHz（stub）；未来加 `AudioResampler` |
| 不同步（音视频漂移） | PTS 用视频帧索引派生；音视频严格同步 |
| 大文件 4GB+ 内存 | 仅用 ring 不加载全部；按需 mmap |

## 四、模块关系

### 被谁调用（上游）

- **`functions/render/RenderJob`**：每帧 `writeAudioChunkIfNeeded`。

### 调用谁（下游）

- **`AudioRingBuffer`**：内部缓冲。
- **`AudioSourceStub`**：源接口（本期固定静音）。
- **未来 `ReplayReader`**：读 `.playback` 音频目录。

### 共享数据

- 无。

### 事件订阅 / 发送

- 无。

## 五、阅读顺序

1. 本文件
2. [functions/render/render-job.md](file:///d:/raplay/Playback/docs/functions/render/render-job.md)
3. [functions/render/frame-encoder.md](file:///d:/raplay/Playback/docs/functions/render/frame-encoder.md)
