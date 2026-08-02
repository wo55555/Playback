# DetailsPanel v3

## 需求

DetailsPanel v3 以统一选择状态驱动，覆盖空状态、摄像机序列、世界Actor、子Actor、摄像机、关键帧和标记。旧 Clip、Transition 与 CameraTrack 属性页不再显示。所有已有可撤销操作均经由 `EditorBridge` 提交，锁定段与锁定摄像机禁用对应操作。

## 架构

`SelectionModel` 提供稳定 id，`DetailsPanel` 在当前帧从 `EditorStateExt` 解析对象并渲染上下文表单，`EditorBridge` 将可用修改转换为命令栈操作。面板不保存对象指针或跨帧索引。世界Actor 的子Actor仅在四个类别折叠组中展示，不生成时间轴行。

## 执行

1. 以 v3 选择类型替换 DetailsPanel 的旧 Clip/Transition 分发。
2. 接入 sequence、worldActor、camera、keyframe 和 marker 的已有 Bridge 命令。
3. 对空对象、失效 id、无摄像机和锁定对象提供解释性状态。
4. 通过 xmake 构建验证主目标。
