# 导出时序 — 端到端视角

> 串联 [editor/export-panel.md](file:///d:/raplay/Playback/docs/editor/export-panel.md) / [editor/camera-track.md](file:///d:/raplay/Playback/docs/editor/camera-track.md) / [editor/export-config.md](file:///d:/raplay/Playback/docs/editor/export-config.md) / [functions/render/render-job.md](file:///d:/raplay/Playback/docs/functions/render/render-job.md) / [functions/render/frame-source.md](file:///d:/raplay/Playback/docs/functions/render/frame-source.md) / [functions/render/frame-encoder.md](file:///d:/raplay/Playback/docs/functions/render/frame-encoder.md) / [functions/render/audio-track.md](file:///d:/raplay/Playback/docs/functions/render/audio-track.md) / [functions/render/export-presets.md](file:///d:/raplay/Playback/docs/functions/render/export-presets.md) / [command/export.md](file:///d:/raplay/Playback/docs/command/export.md)。

## 0. 总体视图

```mermaid
flowchart LR
    subgraph Editor["编辑器侧 (UI)"]
        P["ExportPanel<br/>(ImGui)"]
        CT["CameraTrackPanel"]
    end
    subgraph EC["EditorContext (mutex)"]
        EC0["EditorContextExportExt"]
        EC1["EditorContextCameraExt"]
    end
    subgraph Render["渲染管线"]
        RJ["RenderJob<br/>(状态机)"]
        FS["Dx12FrameSource"]
        AE["AudioTrack"]
        CE["FFmpegPipeEncoder"]
        PIPE["Win32 NamedPipe"]
    end
    subgraph Engine["MCBE + ReplaySession"]
        RS["ReplaySession<br/>(RenderMode=EditorRender)"]
        CAM["CameraManager"]
    end
    subgraph CMD["命令"]
        EX["playback export"]
    end

    P --> EC0
    CT --> EC1
    EX --> EC0
    EX --> RJ
    RJ --> EC0
    RJ --> RS
    RJ --> FS
    RJ --> AE
    RJ --> CE
    CE --> PIPE --> FF["ffmpeg.exe"]
    FS --> CAM
    RS --> CAM
    EC1 --> RJ
```

## 1. 时序：完整流程（成功路径）

```mermaid
sequenceDiagram
    autonumber
    participant U as User
    participant Panel as ExportPanel
    participant EC as EditorContext
    participant Ctrl as EditorController
    participant RJ as RenderJob
    participant RS as ReplaySession
    participant FS as FrameSource
    participant AE as AudioTrack
    participant CE as FrameEncoder
    participant Pipe as NamedPipe
    participant FF as ffmpeg.exe
    participant D3D as D3D12 Present Hook

    Note over U,Panel: 阶段 A — 配置
    U->>Panel: 选 preset = YouTube 1080p
    Panel->>Panel: validate() 通过
    Panel->>EC: submitExportAction(UpdateConfig)
    EC-->>Panel: state 更新

    U->>Panel: 编辑 CameraTrack (insert keyframe)
    Panel->>EC: submitCameraAction(InsertKey)
    EC-->>Panel: snapshot 更新

    U->>Panel: 点 "Start Export"
    Panel->>EC: submitExportAction(Start)
    Note over Ctrl: 下一 tick
    Ctrl->>EC: takeExportActions() → [Start]
    Ctrl->>RJ: submitJob(QueuedJob)

    Note over RJ: 阶段 B — Preparing
    RJ->>RJ: probeEncoders() + buildCommand()
    RJ->>RS: setRenderMode(EditorRender)
    RJ->>RS: requestSeek(inTick)
    RJ->>Pipe: create video pipe
    RJ->>Pipe: create audio pipe
    RJ->>CE: start(config, cmd) → launch ffmpeg
    CE->>FF: CreateProcessW(args)
    FF-->>CE: pid
    RJ->>FS: initialize({width, height})
    FS->>D3D: CreateCommittedResource(RTV + Staging)
    RJ->>FS: tryEncode1Frame()  // 试编码
    FS->>FS: beginFrame + waitForFrame
    D3D-->>FS: signal frameReady
    FS->>FS: captureToStaging
    FS-->>RJ: BGRA 指针
    RJ->>CE: writeVideoFrame(bgra, W*H*4)
    CE->>Pipe: write(bgra, bytes)
    Pipe-->>FF: 读 pipe
    Note over RJ: 试编码成功 → 状态 = Encoding

    Note over RJ,FF: 阶段 C — Encoding (循环)
    loop framesDone < framesTotal
        RJ->>RS: tick() × (20/fps)
        RJ->>RS: getCurrentTick
        RJ->>EC: snapshotCamera() → CameraTrack
        EC-->>RJ: CameraTrackSnapshot
        RJ->>RJ: CameraSampler::sampleAt(tick)
        RJ->>FS: applyCamera(sample) → CameraManager
        Note over FS,D3D: MCBE 渲一帧到 RTV
        D3D-->>FS: frameReady signal
        FS->>FS: captureToStaging
        RJ->>AE: feedTick(tick, dt) + drainFrames
        RJ->>CE: writeVideoFrame(bgra)
        CE->>Pipe: write
        RJ->>CE: writeAudioChunk(pcm, frames)
        CE->>Pipe: write
        RJ->>EC: publishExportState(Encoding, progress)
        EC-->>Panel: 进度条更新
    end

    Note over RJ: 阶段 D — Finalizing
    RJ->>CE: waitForCompletion()
    CE->>Pipe: close video pipe
    CE->>Pipe: close audio pipe
    FF-->>CE: exit
    CE-->>RJ: EncodeResult{success, exitCode=0, outputPath}
    RJ->>RJ: AtomicFileWriter::commit (.tmp → final)
    RJ->>RS: setRenderMode(Live)
    RJ->>FS: shutdown()
    RJ->>EC: publishExportState(Done)
    EC-->>Panel: 显示 "Done"

    Note over U: 完成，可在 VLC 播放
```

## 2. 时序：用户取消

```mermaid
sequenceDiagram
    autonumber
    participant U as User
    participant Panel as ExportPanel
    participant EC as EditorContext
    participant Ctrl as EditorController
    participant RJ as RenderJob
    participant CE as FrameEncoder
    participant FF as ffmpeg.exe

    U->>Panel: 点 "Cancel Export"
    Panel->>EC: submitExportAction(Cancel)
    Ctrl->>EC: takeExportActions()
    Ctrl->>RJ: cancelCurrent()
    RJ->>RJ: stopToken.cancel()
    Note over RJ: 下一帧循环检查
    RJ->>CE: cancel()
    CE->>FF: kill(进程)
    FF-->>CE: 终止
    CE->>CE: abort (.tmp 删)
    RJ->>EC: publishExportState(Cancelled)
    EC-->>Panel: 显示 "Cancelled"
    Note over RJ: 回到 Idle
```

## 3. 时序：命令行入口

```mermaid
sequenceDiagram
    autonumber
    participant U as User
    participant FW as LeviLamina
    participant EX as Export.cpp
    participant EC as EditorContext
    participant RJ as RenderJob

    U->>FW: playback export start data/r.zip --preset youtube --fps 30
    FW->>EX: overload handler
    EX->>EX: parseExportArgs → args
    EX->>EX: resolveConfig(args) → ExportConfig
    EX->>EC: snapshotCamera() → CameraTrack
    EC-->>EX: CameraTrack
    EX->>EX: validate()
    EX->>RJ: submitJob(QueuedJob)
    RJ-->>EX: ok
    EX->>U: "playback.command.export.started"
    Note over RJ: 与上面"阶段 B/C"相同
```

## 4. 关键不变量（端到端）

1. **数据基底唯一**：`ExportConfig` + `CameraTrack` 是唯一输入；无论 ImGui 还是命令行，提交的是同一份结构。
2. **执行唯一**：`RenderJob` 是唯一执行者；UI / 命令行都是 `submitJob` 的调用方。
3. **资源三段释放**：Encoding → Finalizing → Done 路径必走 `cleanupTmp` + `RS::setRenderMode(Live)` + `FS::shutdown`，即便异常。
4. **失败可恢复**：crash 留下 `.exporting.tmp`；启动时扫描并提示用户。
5. **PTS 严格**：音视频 PTS = `frameIndex / fps`；不允许用 wall clock 派生（避免帧率漂移）。

## 5. 阅读顺序

1. 本文件
2. 任意子模块文档
