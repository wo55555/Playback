# 07 · 链路组装

> 入口：`src/playback/editor/ReplayUI.cpp` + `src/playback/refactor/editor/EditorBridge.cpp`
> 角色：将重构后的模块化 UI（`playback::refactor::editor`）接入 legacy 业务链（`playback::editor::EditorContext → EditorController → ReplaySession`）

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| LA-1 | 重构版 Editor 的绘制必须接入 ImGui 渲染循环 | P0 |
| LA-2 | 重构版 Editor 的播放控制（play/pause/seek/speed）必须通过 EditorBridge 路由到 Legacy 业务链 | P0 |
| LA-3 | 重构版 Editor 的编辑命令（split/trim/delete/undo/redo）必须通过 CommandStack 管理 | P0 |
| LA-4 | 键盘快捷键（Space/Ctrl+Z/Home/End 等）必须映射到 EditorBridge 方法 | P0 |
| LA-5 | MCBE 键盘事件必须转发到 ImGui 键盘状态，使快捷键可被检测 | P0 |
| LA-6 | 重构版 Editor 关闭时，回退到 Legacy UI（drawReplayView） | P1 |
| LA-7 | 重构版 Editor 的 Lucide 图标字体必须合并到 Legacy ImGui 字体图集 | P1 |
| LA-8 | Legacy 系统销毁时，EditorBridge 和 Editor 必须正确清理 | P0 |

### 1.2 非功能性需求

- **零侵入 legacy 业务逻辑**：EditorBridge 只通过 `EditorContext::submit()` 和 `EditorContext::snapshot()` 与 legacy 交互
- **线程安全**：键盘事件从 MCBE 线程写入 queue，ImGui 渲染线程读取
- **延迟加载**：Editor 的字体加载延迟到第一个 ImGui 帧，避免在 ImGui 上下文未创建时崩溃

## 二、架构（Architecture）

### 2.1 整体链路图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         MCBE 渲染循环 (D3D12)                          │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │   ImGuiRenderer::render()                                         │   │
│  │                                                                   │   │
│  │   1. ImGui_ImplDX12_NewFrame()                                   │   │
│  │   2. InputHook::syncFrame()       ← 转发键盘事件到 ImGui IO      │   │
│  │   3. beginReplayMouseFrame()      ← 鼠标事件 / 游戏视口坐标     │   │
│  │   4. ImGui::NewFrame()                                           │   │
│  │   5. 绘制游戏视口纹理 (背景)                                      │   │
│  │   6. 判断 Editor.isOpen():                                       │   │
│  │      ├─ true  → Editor::draw()    ← 重构版 UI                    │   │
│  │      └─ false → drawReplayView()  ← Legacy UI (回退)             │   │
│  │   7. endReplayMouseFrame()                                       │   │
│  │   8. ImGui::Render()                                             │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │   Editor::draw()                                                  │   │
│  │                                                                   │   │
│  │   1. 延迟加载字体 (首次)                                          │   │
│  │   2. mTheme.apply()          ← 应用 Editor 主题                   │   │
│  │   3. EditorBridge::syncState()  ← Legacy → 新状态                │   │
│  │   4. mEditMode.draw() / mRenderMode.draw()                       │   │
│  │   5. EditorBridge::commitState() ← 新状态 → Legacy               │   │
│  │   6. ErrorDialog::draw()                                         │   │
│  │   7. handleKeyboardShortcuts()  ← 快捷键路由                      │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │   EditorBridge                                                    │   │
│  │                                                                   │   │
│  │   syncState()  →  EditorContext::snapshot() → EditorStateExt     │   │
│  │   commitState() → EditorContext::submit(action)  (批量)          │   │
│  │   playPause()   → submitAction(TogglePause)                      │   │
│  │   seek(t)       → submitAction(Seek, t)                          │   │
│  │   undo/redo     → CommandStack::undo/redo(state)                 │   │
│  │   split/trim    → CommandStack::push(cmd, state)  + EventBus     │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │   Legacy 业务链                                                    │   │
│  │                                                                   │   │
│  │   EditorContext  ← (线程安全 state + action queue)                │   │
│  │       ↓                                                           │   │
│  │   EditorController::tick()  ← 取 actions → ReplaySession         │   │
│  │       ↓                                                           │   │
│  │   ReplaySession  (setPaused / requestSeek / adjustPlaybackSpeed) │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 模块关系

```
┌──────────────────────┐     syncState/commitState     ┌──────────────────────┐
│  play::refactor::editor│ ◄──────────────────────────► │  play::editor        │
│                        │                              │                       │
│  Editor                │                              │  EditorContext        │
│  EditorBridge          │                              │  EditorController     │
│  CommandStack          │                              │  ReplaySession        │
│  InputHook             │                              │  ImGuiRenderer        │
│  KeyMap                │                              │  ReplayMouseHook      │
│  (panels/*)            │                              │  ReplayUI             │
└──────────────────────┘                              └──────────────────────┘
         │                                                      │
         │  InputHook::onKeyEvent()                              │  handleKeyInput()
         └──────────────────────────────►──────────────────────┘
                (MCBE 键盘事件转发)
```

### 2.3 数据流

```
                     ┌──────────────┐
                     │  User Input  │
                     └──────┬───────┘
                            │
              ┌─────────────▼─────────────┐
              │  键盘: MCBE event → queue │
              │  鼠标: ReplayMouseHook    │
              │  UI: ImGui 控件点击       │
              └─────────────┬─────────────┘
                            │
                    ┌───────▼───────┐
                    │  EditorBridge  │
                    └───────┬───────┘
                            │
            ┌───────────────┼───────────────┐
            │               │               │
    ┌───────▼──────┐ ┌──────▼──────┐ ┌──────▼──────┐
    │ 播放控制      │ │ 编辑命令    │ │  Undo/Redo  │
    │  → action    │ │  → Command  │ │  → Command  │
    │  → Context   │ │  → Stack    │ │  → Stack    │
    └───────┬──────┘ └──────┬──────┘ └──────┬──────┘
            │               │               │
    ┌───────▼───────────────▼───────────────▼──────┐
    │          EditorContext                       │
    │          (thread-safe action queue)          │
    └───────────────────────┬──────────────────────┘
                            │
                    ┌───────▼───────┐
                    │  Controller   │
                    │  ::tick()     │
                    └───────┬───────┘
                            │
                    ┌───────▼───────┐
                    │  ReplaySession│
                    │  (MCBE 回放)  │
                    └───────────────┘
```

## 三、执行（Execution）

### 3.1 初始化流程

```
hookReplayUI(true)
  │
  ├─ gContext.reset()
  ├─ gImGuiRenderer.setContext(&gContext)
  ├─ setReplayUIActive(true)
  ├─ EditorBridge::getInstance().initialize(&gContext)   ← 桥接层初始化
  ├─ Editor::getInstance().initialize()                   ← 编辑器初始化 (KeyMap + Theme)
  ├─ hookRendererInit(true)  → D3D12 早期 Hook
  ├─ hookD3D12(true)         → Present / Resize 拦截
  └─ hookReplayMouse(true)  → 鼠标/键盘事件 Hook
```

### 3.2 每帧流程

```
tickReplayUI(hudVisible)
  └─ EditorController::tick(hudVisible)
       ├─ takeActions() → 处理前帧的 EditorAction
       │    ├─ TogglePause  → session.setPaused()
       │    ├─ Seek         → session.requestSeek()
       │    ├─ SkipToStart  → session.requestSeek(0)
       │    ├─ SkipToEnd    → session.requestSeek(total)
       │    ├─ DecreaseSpeed→ session.adjustPlaybackSpeed(-1)
       │    ├─ IncreaseSpeed→ session.adjustPlaybackSpeed(1)
       │    └─ StopReplay   → session.requestStop()
       └─ publish(EditorState) → 更新 context 中的状态快照
```

### 3.3 渲染帧流程

```
ImGuiRenderer::render(swapChain)
  │
  ├─ ImGui_ImplDX12_NewFrame()
  ├─ InputHook::syncFrame()       ← 从 queue 读出键盘事件 → ImGui::AddKeyEvent
  ├─ beginReplayMouseFrame()      ← 从 queue 读出鼠标事件 + 焦点
  ├─ ImGui::NewFrame()
  ├─ 绘制游戏视口 (背景纹理)
  │
  ├─ if Editor::isOpen():
  │    └─ Editor::draw()
  │         ├─ 延迟加载字体 (首次)
  │         ├─ mTheme.apply()
  │         ├─ EditorBridge::syncState(mState)   ← 从 EditorContext 读状态
  │         ├─ mEditMode.draw() / mRenderMode.draw()
  │         ├─ EditorBridge::commitState()       ← 批量提交 actions
  │         ├─ ErrorDialog::draw()
  │         └─ handleKeyboardShortcuts()
  │
  ├─ else:  ← Legacy 回退
  │    └─ ui::drawReplayView(state, layout, actions)
  │         └─ for action in actions: p.editorContext->submit(action)
  │
  ├─ endReplayMouseFrame()
  └─ ImGui::Render()
```

### 3.4 键盘快捷键映射

| 快捷键 | EditorBridge 方法 | 功能 |
|---|---|---|
| `Space` | `playPause()` | 播放/暂停 |
| `Home` | `skipToStart()` | 跳转到开头 |
| `End` | `skipToEnd()` | 跳转到结尾 |
| `←` | `seek(currentTick - 1)` | 逐帧后退 |
| `→` | `seek(currentTick + 1)` | 逐帧前进 |
| `Shift+←` | `seek(currentTick - 20)` | 后退 1 秒 |
| `Shift+→` | `seek(currentTick + 20)` | 前进 1 秒 |
| `-` | `decreaseSpeed()` | 减速 |
| `=` | `increaseSpeed()` | 加速 |
| `Ctrl+Z` | `undo(state)` | 撤销 |
| `Ctrl+Y` | `redo(state)` | 重做 |
| `Del` | — (由 TimelinePanel 处理) | 删除选中 Clip |
| `Esc` | `mSelection.clear()` | 取消选择 |
| `F1` | `Editor::toggle()` | 切换编辑器显隐 |

### 3.5 键盘事件转发 (MCBE → ImGui)

```
MCBE: KeyInputEvent
  │
  └─ ReplayMouseHook::handleKeyInput()
       └─ InputHook::onKeyEvent(keyCode, down)
            └─ gKeyQueue.push({keyCode, down})   ← 线程安全队列
                                                   │
ImGuiRenderer::render()                            │
  └─ InputHook::syncFrame()  ◄────────────────────┘
       └─ while queue not empty:
            io.AddKeyEvent(vkToImGuiKey(code), down)
```

### 3.6 命令执行 (Edit Commands)

```
用户操作 (UI 点击 / 拖拽)
  │
  ├─ TimelinePanel::handleSplitAtPlayhead()
  │    └─ EditorBridge::splitClip(state, trackId, clipId, tick)
  │         ├─ new SplitClipCommand(trackId, clipId, tick)
  │         ├─ mCommandStack.push(cmd, state)  ← 执行 + 入栈
  │         └─ EventBus::emit(CommandExecutedEvent)
  │
  ├─ TimelinePanel::handleRippleDelete()
  │    └─ EditorBridge::deleteClip(state, trackId, clipId)
  │         ├─ new RemoveClipCommand(trackId, clipId)
  │         ├─ mCommandStack.push(cmd, state)  ← 执行 + 入栈
  │         └─ EventBus::emit(CommandExecutedEvent)
  │
  ├─ MenuBar "Undo" 点击
  │    └─ EditorBridge::undo(state)
  │         └─ mCommandStack.undo(state)  ← 出栈 + 撤销
  │
  └─ MenuBar "Redo" 点击
       └─ EditorBridge::redo(state)
            └─ mCommandStack.redo(state)  ← 出栈 + 重做
```

### 3.7 字体集成

```
Legacy ImGuiRenderer::Impl::init()
  │
  ├─ 加载 msyh.ttc (中文字体, 13px)
  ├─ 加载 lucide.ttf (图标字体, 合并模式, PUA 范围 0xe000-0xe6ff)
  │   └─ 确保 IconSystem 的 ICON_* 宏能正确渲染
  └─ io.Fonts->Build()
```

### 3.8 清理流程

```
hookReplayUI(false)
  │
  ├─ setReplayUIActive(false)
  ├─ Editor::getInstance().shutdown()
  │    ├─ mSelection.clear()
  │    ├─ mState = {}
  │    └─ EditorBridge::shutdown()
  │         ├─ mContext = nullptr
  │         ├─ mCommandStack.clear()
  │         └─ mPendingActions.clear()
  │
  ├─ hookRendererInit(false)
  ├─ hookD3D12(false)
  ├─ hookReplayMouse(false)
  │
  └─ gImGuiRenderer.setContext(nullptr)
     └─ gContext.reset()
```

## 四、模块关系

### 4.1 依赖关系

| 模块 | 依赖 | 方向 |
|---|---|---|
| `EditorBridge` | `EditorContext` (legacy) | refactor → legacy |
| `EditorBridge` | `CommandStack` | refactor 内部 |
| `EditorBridge` | `EventBus` | refactor 内部 |
| `Editor` | `EditorBridge` | refactor 内部 |
| `Editor` | `EditMode` / `RenderMode` | refactor 内部 |
| `InputHook` | `Editor` | refactor 内部 |
| `ImGuiRenderer` | `Editor` / `EditorBridge` / `InputHook` | legacy → refactor |
| `ReplayUI` | `EditorBridge` / `Editor` | legacy → refactor |
| `ReplayMouseHook` | `InputHook` | legacy → refactor |

### 4.2 文件清单

| 文件 | 角色 |
|---|---|
| `src/playback/refactor/editor/EditorBridge.h/.cpp` | 桥接层：状态同步 + 播放控制 + 编辑命令 |
| `src/playback/refactor/editor/CommandStack.h/.cpp` | 撤销/重做栈 |
| `src/playback/refactor/editor/InputHook.h/.cpp` | 键盘事件队列 + ImGui 转发 |
| `src/playback/refactor/editor/Editor.h/.cpp` | 主编辑器：生命周期 + 绘制 + 快捷键 |
| `src/playback/editor/ReplayUI.cpp` | Legacy 初始化入口 |
| `src/playback/editor/renderer/ImGuiRenderer.cpp` | 渲染循环集成 |
| `src/playback/editor/renderer/ReplayMouseHook.cpp` | 键盘事件转发 |

## 五、阅读建议

1. **先看架构图**（§2.1）了解整体链路
2. **看初始化流程**（§3.1）了解 EditorBridge 何时接入
3. **看渲染帧流程**（§3.3）了解每帧的绘制链路
4. **看命令执行**（§3.6）了解编辑操作如何路由

## 六、变更日志

| 日期 | 变更 |
|---|---|
| 2026-07-30 | 初稿：链路组装需求 + 架构 + 执行流程 |