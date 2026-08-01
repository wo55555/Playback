# 回放浏览器骨架接入

## 需求

- 主菜单的回放入口打开 ImGui 全屏回放目录，不再创建旧 JSON 弹窗。
- 浏览器打开时覆盖整个客户区，优先于编辑器与旧时间线绘制，从视觉上隐藏主界面 UI。
- 骨架阶段提供返回关闭、加载目录、搜索、平铺文件卡片、选中操作栏与打开回放；导入、编辑、删除保留禁用入口。

## 架构

- `ReplayBrowserWindow` 是唯一的 ImGui 浏览器状态持有者，复用 `screen::ReplayBrowser` 的列表加载、筛选匹配和打开接口。
- `MainMenuHooks` 只负责把主菜单按钮请求转发给 `ReplayBrowserWindow::open()`。
- `ImGuiRenderer` 在两条 D3D 渲染路径上将浏览器设为最高绘制优先级，打开时不绘制 Editor 或旧时间线。
- `ReplayMouseHook` 与 `InputHook` 以浏览器打开状态为 UI 输入所有权依据。

## 执行

1. 新增 `refactor/replay-browser/ReplayBrowserWindow`，实现全屏窗口与平铺目录骨架。
2. 将主菜单 tick 的延迟打开逻辑切换为新窗口。
3. 在 D3D11、D3D12 帧中优先绘制浏览器，并将游戏视口置空以阻止游戏场景输入。
4. 扩展键盘和鼠标输入所有权判定，关闭时恢复现有 UI 与游戏输入。
5. 编译 `Playback`，确认新增源文件被 xmake 的递归规则收集。
