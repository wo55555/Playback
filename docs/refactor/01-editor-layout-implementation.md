# 编辑器嵌入式布局实现

## 需求

- 编辑器所有面板在游戏窗口内绘制，不使用 DockBuilder、ImGui 多视口或独立窗口。
- Details 宽度与 Timeline 高度通过两个显式分隔条独立调整，分别受最小 Viewport 尺寸约束。
- Viewport 使用固定视频比例计算画面区域，容器剩余区域绘制为黑边。
- 视频比例由编辑器的用户视频设置保存和驱动，不从游戏交换链尺寸推导。
- 导出配置只能由 `File > Export...` 打开；Viewport 和顶级菜单不提供导出入口。
- 所有用户可见文本字号不小于 14px。

## 架构

- `Editor` 保存并持久化 `mDetailsWidthRatio`、`mTimelineHeightRatio` 与 `mVideoAspectRatio`。
- `EditMode` 先计算 Menu、Status、右侧 Details、左侧 Timeline 与剩余 Viewport 容器，再在两个边界调用 `Splitter`。
- `Splitter` 提供水平和垂直两个无停靠语义的拖动控件，返回已经按边界钳制的比例。
- `ViewportPanel` 直接在 EditMode 分配的容器内绘制，按固定比例计算黑边画面区域，并仅在该区域接收视口操作。
- `MenuBar` 将导出动作收敛在 File 子菜单，先打开导出配置模态；确认 Start 后才进入 Render 模式。

## 执行

1. 将 Viewport、Timeline、Curve Editor 改为嵌入外层固定容器。
2. 扩展布局状态与 Splitter API，在 `EditMode` 接入双分隔条及曲线面板的最小宽度约束。
3. 更新 Viewport 的用户比例设置、画面矩形和交互命中边界。
4. 持久化布局比例与视频比例，并保证所有自绘文字字号不低于 14px。
5. 构建 Playback，并检查编译错误和布局边界。
