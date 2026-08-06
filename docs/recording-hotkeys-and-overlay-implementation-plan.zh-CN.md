# 录制快捷键与状态层实施计划

## 需求

本计划落实 [录制快捷键与状态层开发文档](recording-hotkeys-and-overlay.zh-CN.md) 中已确认的能力：普通游戏世界支持 `P` 开始/结束录制、`L` 暂停/继续录制；录制活动时显示无背景的 `● 录制中 3:25` 状态层；玩家可在回放浏览器设置入口调整键位、显示开关和四角位置。

本期不改变既有 `/record` 命令或回放编辑器快捷键，不处理组合键和拖拽定位。

## 架构

### 当前链路与落点

| 链路 | 现有落点 | 本次接入方式 |
| --- | --- | --- |
| 录制状态机 | `functions/record/Recorder` | 增加公开、只读的录制状态与累计有效时长快照；所有状态变化仍由 `Recorder` 的既有 `start/pause/stop` 完成。 |
| 客户端场景判定 | `functions/tick/ClientTickHooks.cpp` | 提取并发布普通游戏 HUD 可见状态；它作为快捷键和状态层的共同前置条件。 |
| 键盘输入 | `editor/renderer/ReplayMouseHook.cpp` 的 `KeyInputEvent` 监听器 | 新建运行时快捷键控制器并在此分发前调用；仅在事件按下边沿、无 UI 输入占用、普通游戏 HUD 可见时处理。 |
| 配置 | `Config.h` 与模组配置读写路径 | 新增 `recordingControls` 配置段，保存两个虚拟键、显示开关和位置枚举；版本迁移缺字段时回填默认值。 |
| 渲染 | `editor/renderer/ImGuiRenderer.cpp` 的 D3D11 与 D3D12 帧路径 | 在两个 `ImGui::NewFrame()` 后调用同一个状态层渲染器，避免任一图形后端缺失。 |
| 设置入口 | `screen/select_replay/SelectReplayScreen.cpp` 的设置弹窗 | 在现有齿轮菜单增加“录制设置”，打开新的 ImGui 模态面板。 |

### 目标文件清单

| 操作 | 文件 |
| --- | --- |
| 修改 | `src/playback/Config.h` |
| 修改 | `src/playback/functions/record/Recorder.h`、`Recorder.cpp` |
| 新增 | `src/playback/functions/record/RecordingControls.h`、`RecordingControls.cpp` |
| 修改 | `src/playback/functions/tick/ClientTickHooks.cpp` |
| 修改 | `src/playback/editor/renderer/ReplayMouseHook.cpp` |
| 新增 | `src/playback/editor/ui/RecordingSettingsPanel.h`、`RecordingSettingsPanel.cpp` |
| 新增 | `src/playback/editor/ui/RecordingStatusOverlay.h`、`RecordingStatusOverlay.cpp` |
| 修改 | `src/playback/editor/renderer/ImGuiRenderer.cpp` |
| 修改 | `src/playback/screen/select_replay/SelectReplayScreen.cpp`（及其头文件，如该类需保存弹窗状态） |
| 修改 | `src/lang/zh_CN.json`、`src/lang/en_US.json` |
| 修改 | `xmake.lua` 仅在新增测试目标或源文件未被现有 `src/**.cpp` 通配规则覆盖时；正常情况无需变更。 |

## 执行

### 步骤 1：定义配置模型与持久化契约

1. 在 `Config.h` 定义 `RecordingOverlayPosition`（`TopLeft`、`TopRight`、`BottomLeft`、`BottomRight`）和 `RecordingControlsConfig`。
2. 设置默认值：`toggleRecordingKey = 'P'`、`togglePauseKey = 'L'`、`showStatusOverlay = true`、`overlayPosition = TopLeft`。
3. 将该配置作为 `Config::recordingControls` 字段，递增配置版本，并确认模组原有配置加载/保存机制能够序列化枚举和虚拟键值；如不能，提供字符串与 `UINT` 的显式转换。
4. 在配置加载后执行迁移与校验：键值为空、保留键、不支持键或两个动作相同，均回退到对应默认值并写警告日志。
5. 在 `RecordingControls` 中集中提供 `defaults()`、`validate()`、`keyDisplayName()` 和 `save()`，设置 UI 与输入控制器不得各自实现校验规则。

完成条件：首次升级用户得到 `P/L/左上角` 默认配置；保存后重启仍能完整恢复；无效配置不阻塞加载。

### 步骤 2：把录制状态与有效时长收敛到 Recorder

1. 在 `Recorder.h` 将可读状态以公开 `RecordingState` 枚举或等价只读快照暴露，避免外部通过 `isActive()` 和 `isPaused()` 组合推断 `Closing`。
2. 增加 `RecordingStatusSnapshot { state, elapsed }` 以及 `getStatusSnapshot()`；`elapsed` 使用 `std::chrono::steady_clock`，且返回秒粒度。
3. 在 `start()` 成功进入录制态时初始化本次有效时段起点和累计时长；在 `pause()` 两个方向切换时累加/恢复计时；在 `stop()`、`failRecording()`、`cancelRecording()`、世界退出清理中冻结或重置计时状态。
4. 保持 `endTick()`、网络包采集、分块保存和原有命令行为不变；新快照仅为读取状态，不触发写盘或状态迁移。
5. 格式化 `m:ss` 留在 UI 层，`Recorder` 只暴露时长值，保证业务逻辑与展示分离。

完成条件：录制时长递增，暂停时静止，继续后累计，停止或失败后快照不再触发状态层显示。

### 步骤 3：建立场景门禁和全局按键控制器

1. 新建 `RecordingControls` 模块，提供 `setGameHudVisible(bool)`、`onKeyInput(int key, bool isDown, bool uiOwnsKeyboard)`、`resetPressedKeys()` 与 `isInteractionAllowed()`。
2. 在 `ClientTickHooks.cpp` 复用已有 `hudVisible` 判定，将其每帧推送给控制器；当 HUD 不可见、模式不是 `PlaybackMode::Record`、离开世界或进入菜单时清空已按下集合，防止跨场景的按键释放造成错误触发。
3. 在 `ReplayMouseHook.cpp::handleKeyInput()` 的最前面，先交给录制控制器处理；仅 `isDown == true` 且该键此前未被按住时产生动作。键盘事件的后续 ImGui/编辑器路由维持原逻辑。
4. 门禁顺序固定为：普通录制世界 → HUD 可见 → 游戏未被编辑 UI 捕获 → 键位匹配 → 去重边沿 → 调用 `Recorder`。
5. 状态映射固定为：`P` 在 `Idle` 调 `start()`，在 `Recording/Paused` 调 `stop()`；`L` 在 `Recording/Paused` 调 `pause()`，其余状态忽略。只有调用后快照确实变化时才消费事件并写调试日志。
6. 明确与编辑器隔离：回放浏览器、回放编辑器、文本输入、弹窗键位捕获期间不触发游戏录制快捷键；编辑器的 `KeyMap` 不纳入本模块。

完成条件：按住 `P` 或 `L` 不会重复切换；菜单、回放、加载页和编辑 UI 都不会触发控制；既有编辑器键盘路由不回归。

### 步骤 4：实现无背景录制状态层

1. 新建 `RecordingStatusOverlay`，输入为 `RecordingStatusSnapshot`、`RecordingControlsConfig`、显示尺寸和 HUD 可见标志；输出只包含 ImGui 前景绘制。
2. 显示条件必须同时满足：配置启用、HUD 可见、普通录制世界、`state == Recording || state == Paused`。
3. 以 `ImGui::GetForegroundDrawList()` 绘制红色圆点与文本；可以使用文本阴影提升可读性，但禁止 `AddRectFilled`、边框、窗口背景、padding 卡片或独立 ImGui 窗口。
4. 录制中显示 `● 录制中 m:ss`，暂停显示 `● 已暂停 m:ss`；位置以安全边距锚定到四角，右侧位置用文本宽度右对齐，底部位置以文本高度上移。
5. 在 `ImGuiRenderer.cpp` 的 D3D11 与 D3D12 渲染分支中，分别在 `ImGui::NewFrame()` 后调用同一渲染函数；不可依赖 `state.editorVisible`，以便普通游戏录制时也能显示。
6. 保持现有“capture-only pass”约束：状态层只在 ImGui 后端已初始化且当前渲染分支实际绘制时显示；渲染器不可用不影响快捷键与录制保存。

完成条件：两个图形后端均显示同样的无背景文字层；暂停时间不动；更改位置后四角锚定准确；非允许场景不绘制。

### 步骤 5：接入回放浏览器的录制设置面板

1. 新增 `RecordingSettingsPanel`，由回放浏览器设置菜单的“录制设置”项打开；面板自身保有草稿配置、当前捕获字段、字段错误和打开时的原始版本。
2. 首次打开从已保存配置复制草稿；关闭或取消不落盘。恢复默认只更新草稿；保存调用 `RecordingControls::validate()`，成功后更新运行时配置并持久化。
3. 键位捕获按钮在点击后显示“按下任意按键”；下一次有效 `KeyInputEvent` 写入对应草稿字段并终止捕获。捕获期间必须取消该键的游戏输入和录制快捷键处理，`Escape` 取消捕获而不修改草稿。
4. 设置内容按草图实现：开始/结束录制键、暂停/继续录制键、状态显示开关、位置下拉框、恢复默认、取消和保存；设置页不增加未确认的显示样式选项。
5. 保存前显示字段级错误：键位相同、空键、保留键或不支持键。保存成功后当前游戏内状态层立即按新开关和新位置生效。
6. 面板继续使用现有 ImGui 样式和回放浏览器的锚定弹窗/模态模式，不新增第二套 UI 框架。

完成条件：玩家能在既有设置入口打开面板，修改 `P/L`、开关和四角位置；取消无副作用，恢复默认可保存，非法配置不可保存。

### 步骤 6：补齐本地化与可观测性

1. 在 `zh_CN.json`、`en_US.json` 中补充录制设置标题、两个动作名称、状态显示、位置名称、保存/取消/恢复默认、按键捕获提示和校验错误。
2. 状态层文案复用本地化键，禁止硬编码中文；时长保持数字格式一致。
3. 在配置迁移失败、按键动作调用无效、渲染后端不可用等降级路径使用现有 `Playback` 日志记录一次清晰错误或警告；不记录玩家路径、内容或敏感数据。

完成条件：中英文 UI 不出现缺失键；问题可通过既有模组日志诊断。

### 步骤 7：分层验证与交付检查

1. 为纯逻辑提取可测函数：配置验证/迁移、按键状态转换、去重按键集合、累计有效时长计算、四角锚点和 `m:ss` 格式化。
2. 增加或运行仓库现有测试机制；若当前工程没有测试目标，则至少用专用的编译时可测单元及手动测试清单验证，不引入未被项目使用的测试框架。
3. 执行 `xmake build`，修复所有 C++20、Windows 输入类型和 ImGui 头文件问题；确认 `src/**.cpp` 自动加入目标，避免漏编新文件。
4. 手动测试普通世界：首次 `P` 开始、`L` 暂停、`L` 继续、`P` 结束；检查录制文件、时长和命令兼容性。
5. 手动测试场景隔离：按住键、打开聊天/菜单、加载、离开世界、进入回放浏览器与回放编辑器，确认不会错误触发或遗留状态层。
6. 手动测试设置：变更两个键、四个位置、关闭显示、取消、恢复默认、重启后持久化，以及重复键/保留键的报错。
7. 用 D3D11 与 D3D12 可用环境分别检查状态层；若某后端当前无法初始化，记录为环境限制，同时确认快捷键和录制链路独立可用。

完成条件：全部验收项通过，构建无新增警告，且不存在仅在某个输入、渲染或配置分支生效的断链。

### 实施顺序与依赖

按“配置 → Recorder 状态快照 → 场景门禁/快捷键 → 状态层 → 设置页 → 本地化 → 验证”顺序实施。前两步建立唯一数据源，第三步将输入安全地接入既有业务逻辑，第四、五步分别消费该数据源，因此不在 UI 层复制录制状态或持久化逻辑。每一步完成后先编译并验证自身链路，再进入依赖它的下一步。
