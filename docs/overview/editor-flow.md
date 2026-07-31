# 编辑器时序

## 总览

编辑器由 4 部分组成，本质是一个"游戏画面的 ImGui 浮层"：

| 部分 | 职责 |
| --- | --- |
| `EditorContext` | 中央状态机：保存最新 `EditorState`，收集待消费 `EditorAction` |
| `EditorController` | 每 tick 把 `EditorAction` 翻译成 `ReplaySession` 调用，再把会话状态 publish 成 `EditorState` |
| `ImGuiRenderer` + `D3D12Hooks` | 钩住 `IDXGISwapChain::Present`，把 `backBuffer` 拷到自己的 `gameTexture`，叠加 ImGui 帧 |
| `ui::ReplayView`（MenuBar + Timeline） | 渲染时间轴控件，输出 `EditorAction` 给 Context |

## 完整时序图

```mermaid
sequenceDiagram
    autonumber
    participant PB as Playback::enable
    participant D3D as D3D12Hooks
    participant ReplayUI as editor::ReplayUI<br/>(gContext + gController)
    participant CT as ClientTickHooks
    participant ECtrl as EditorController
    participant EC as EditorContext
    participant RS as ReplaySession
    participant Swap as IDXGISwapChain::Present
    participant IR as ImGuiRenderer<br/>(gImGuiRenderer)
    participant View as ui::ReplayView<br/>(MenuBar + Timeline)
    participant U as 用户

    rect rgba(120,180,250,0.18)
    note over PB,D3D: 1. 装载 (load/enable)
    PB->>D3D: hookReplayUIRendererInit(true)<br/>(LL_TYPE_INSTANCE_HOOK on RendererContextD3D12::init)
    D3D-->>ReplayUI: 在 bgfx init 前预先装好 8 个 IDXGI vtable 钩子
    PB->>ReplayUI: hookReplayUI(true)
    ReplayUI->>EC: gContext.reset()
    ReplayUI->>IR: gImGuiRenderer.setContext(&gContext)
    ReplayUI->>D3D: hookD3D12(true)
    D3D-->>D3D: installAll: Present/Present1/<br/>ResizeBuffers/ResizeBuffers1/<br/>4 个 CreateSwapChain*
    ReplayUI->>D3D: hookReplayMouse(true)
    end

    rect rgba(120,250,160,0.18)
    note over CT,EC: 2. 每个 client tick 推进状态机
    loop ClientTickHooks::_subTick
        CT->>ECtrl: EditorController::tick(hudVisible)
        ECtrl->>EC: takeActions() (消费用户操作)
        loop 每个 action
            ECtrl->>RS: setPaused / requestSeek /<br/>adjustPlaybackSpeed / requestStop
        end
        ECtrl->>RS: 读 isActive / isPaused / getPlaybackSpeed /<br/>getCurrentTick / getTotalTicks / hasJoinedReplayWorld
        ECtrl->>EC: publish(EditorState)
    end

    CT->>ECtrl: tick(hudVisible) — 顶层 HUD 可见性
    ECtrl->>EC: publish(hudVisible)
    end

    rect rgba(250,220,120,0.18)
    note over Swap,IR: 3. 每帧 Present 钩子
    Swap->>D3D: presentDetour(swapChain, sync, flags)
    D3D->>IR: gImGuiRenderer.render(swapChain)
    IR->>EC: editorContext->snapshot() -> EditorState
    alt 状态 replayVisible = false 或 hudVisible = false
        IR-->>D3D: 关闭 mouse input, 必要时 shutdown
    else 可见
        IR->>IR: ImGui_ImplDX12_NewFrame
        IR->>IR: beginReplayMouseFrame(layout)
        IR->>View: drawReplayView(state, layout, actions)
        View-->>U: MenuBar / Timeline 控件 (ImGui)
        U->>U: 点击 / 拖动 timeline / 切档
        View-->>IR: std::vector<EditorAction> actions
        IR->>EC: editorContext->submit(action)
        IR->>IR: endReplayMouseFrame()
        IR->>IR: ImGui::Render + ImGui_ImplDX12_RenderDrawData
        IR->>IR: CopyResource(gameTexture <- backBuffer)
        IR->>IR: ClearRenderTargetView + Draw ImGui
        IR->>IR: Signal fence (提交到 GPU)
    end
    D3D->>Swap: gOriginalPresent(swapChain, ...)
    D3D->>IR: afterPresent (错误时 shutdown)
    end

    rect rgba(250,120,160,0.18)
    note over PB,IR: 4. 卸载
    PB->>ReplayUI: hookReplayUI(false)
    ReplayUI->>D3D: hookD3D12(false) -> removeAll
    D3D->>IR: gImGuiRenderer.shutdown()
    D3D->>D3D: unbind swap chain queue
    ReplayUI->>D3D: hookReplayMouse(false)
    ReplayUI->>D3D: hookReplayUIRendererInit(false)
    ReplayUI->>IR: gImGuiRenderer.setContext(nullptr)
    end
```

## 关键细节

### EditorContext：唯一的可变状态

```cpp
// snapshot() — 渲染线程用
// publish(state) — Controller 写
// submit(action) — Renderer 写
// takeActions() — Controller 读
```

互斥锁保护。Renderer 永远只读 `snapshot`，Controller 永远只 `takeActions` + `publish`，避免锁竞争。

### EditorAction 类型

```cpp
enum class EditorActionType {
    TogglePause,   Seek,           SkipToStart,
    SkipToEnd,     DecreaseSpeed,  IncreaseSpeed,
    StopReplay,
};
struct EditorAction { EditorActionType type; int tick; };
```

`Seek` 才需要 `tick`；其它只看 `type`。

### D3D12 钩子 8 个 vtable

| 钩子 | 作用 |
| --- | --- |
| `IDXGISwapChain::Present` | 主渲染入口，每帧拷贝 backBuffer |
| `IDXGISwapChain1::Present1` | Win8+ Present1，参数有 `DXGI_PRESENT_PARAMETERS` |
| `IDXGISwapChain::ResizeBuffers` | 释放资源、重新 init |
| `IDXGISwapChain3::ResizeBuffers1` | 同上但带 `IUnknown* const* presentQueues`；存 queue 给后续使用 |
| `IDXGIFactory::CreateSwapChain` | 创建时 `bindSwapChainQueue(swapChain, device)` |
| `IDXGIFactory2::CreateSwapChainForHwnd` | 同上（窗口模式） |
| `IDXGIFactory2::CreateSwapChainForCoreWindow` | 同上（UWP 模式） |
| `IDXGIFactory2::CreateSwapChainForComposition` | 同上（合成模式） |

> 注意：`hookReplayUIRendererInit` 是 `LL_TYPE_INSTANCE_HOOK` 钩 `bgfx::d3d12::RendererContextD3D12::init`，保证在 bgfx 真正初始化之前 8 个 vtable 钩子已经装好。这样后面 `CreateSwapChain` 被 bgfx 调用时能直接拿到 queue 句柄。

### ImGuiRenderer 资源

- 私有的 `device` / `commandQueue` / `rtvHeap` / `srvHeap` / `fence` / `fenceEvent`。
- 每帧：拷 `backBuffer` → `gameTexture`（PIXEL_SHADER_RESOURCE） → `Clear` → `ImGui_ImplDX12_RenderDrawData`。
- SRV 描述符从自己 32 个槽中分配（`SrvDescriptorCount = 32`）。
- 字体从 `C:\Windows\Fonts\msyh.ttc` 加载简中常用字符（`GetGlyphRangesChineseSimplifiedCommon`）。
- `SwapChainReplacementDelay = 500ms`：在 swapChain 替换时（如窗口大小改变）给旧 swapChain 500ms 收尾时间，避免 surface area 缩小时被误判。

### 鼠标钩子

`ReplayMouseHook` 独立于 ImGuiRenderer。当 `setReplayUIActive(true)` 时接管鼠标，ImGui 自己处理；时间轴区域外的事件透传给游戏。`beginReplayMouseFrame(layout, w, h)` 通知钩子"这一帧 UI 区域在哪里"。

### HUD 可见性

`ClientTickHooks::PlaybackClientUpdateHook` 判定 HUD 可见性：

```cpp
hudVisible = (topScene & SceneType::HudScene) != 0
          && isInWorldAndNotShowingAnyMenuScreens()
          && !isShowingLoadingScreen()
          && !isShowingProgressScreen();
```

只有同时满足这 4 个条件才显示 ImGui，避免在主菜单、暂停菜单、加载界面打架。

## 编辑器相关模块

- 编辑器总览：[editor/index.md](../editor/index.md)
- 状态机：[editor/context.md](../editor/context.md)
- 控制器：[editor/controller.md](../editor/controller.md)
- 渲染层（D3D12 + ImGui）：[editor/renderer.md](../editor/renderer.md)
- UI 面板：[editor/ui.md](../editor/ui.md)
- 入口与生命周期：[playback/lifecycle.md](../playback/lifecycle.md)
