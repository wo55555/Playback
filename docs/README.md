# Playback 架构文档

Playback 是 Minecraft Bedrock Edition 的客户端回放 mod，构建在 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 之上。它能在本地世界或多人服务器录制客户端可见状态，把录制打包成可移植回放文件，并通过主菜单浏览器在隔离的回放世界中播放。

> 🚀 **一页快速解析**：[overview.md](overview.md) —— 1 张分层架构图 + 1 张录制时序 + 1 张回放时序，5 分钟看完整个项目。

## 快速解析

> 想最短时间理解这个项目？下面这张表先把"做了什么"和"在哪做的"对齐。

| 能力 | 入口 | 关键实现 |
| --- | --- | --- |
| 录制世界 | `record start` / `pause` / `stop` 命令 | [functions/record/record.md](functions/record.md) + [overview/record-flow.md](overview/record-flow.md) |
| 异步落盘 + 区块去重 | 后台 writer 线程 + Snappy 压缩 | [functions/io.md](functions/io.md) |
| 导出回放 `.zip` | `Recorder::saveRecording` → `ReplayExporter` | [functions/record.md](functions/record.md) |
| 主菜单打开回放 | `Playback` 按钮 → 回放浏览器 | [screen/replay-browser.md](screen/replay-browser.md) |
| 在回放世界里回放 | 隔离世界 + Action 重放 + 区块流式注入 | [functions/replay.md](functions/replay.md) + [overview/replay-flow.md](overview/replay-flow.md) |
| 播放/暂停/seek/变速 | 编辑器上下文状态机 | [editor/context.md](editor/context.md) + [editor/controller.md](editor/controller.md) |
| 游戏中 ImGui 时间轴 | D3D12 hook + 帧拷贝 | [editor/renderer.md](editor/renderer.md) + [overview/editor-flow.md](overview/editor-flow.md) |
| 录制/回放的"协议"层 | Action 注册中心 + 二进制协议 | [functions/action.md](functions/action.md) |

> 一个核心心智模型：**Recorder 和 ReplaySession 共享同一套 Action 协议**。录制把游戏事件写成 Action 二进制流；回放从同一份 Action 二进制流重建游戏事件。chunk 走单独的压缩缓存以避免重复写盘。

## 文档地图

```
docs/
├── README.md                       本文件（快速解析 + 文档地图）
├── overview.md                     🚀 一页快速解析（3 张图：分层 + 录制 + 回放）
├── overview/                       整体视角
│   ├── architecture.md             分层 + 模块依赖图
│   ├── record-flow.md              录制时序图
│   ├── replay-flow.md              回放时序图
│   └── editor-flow.md              编辑器时序图
├── playback/                       模组入口
│   ├── index.md                    Playback 单例 + 生命周期
│   ├── config.md                   Config 与 CommandConfigStruct
│   └── lifecycle.md                load/enable/disable/hook/unhook 状态机
├── command/                        命令层
│   └── index.md                    playback / record 命令族
├── functions/                      核心功能
│   ├── index.md                    总览 + 子模块关系
│   ├── action.md                   Action 注册中心 + 协议
│   ├── record.md                   Recorder / ReplayExporter / 录制主流程
│   ├── io.md                       AsyncReplaySaver / ReplayWriter / ReplayReader / 缓存
│   ├── replay.md                   ReplaySession 状态机 + 区块流式注入
│   └── tick.md                     ClientTickHooks
├── editor/                         回放编辑器
│   ├── index.md                    编辑器子模块关系图
│   ├── context.md                  EditorContext 状态机
│   ├── controller.md               EditorController
│   ├── renderer.md                 D3D12 钩子 + ImGui 渲染
│   └── ui.md                       ReplayView / MenuBar / Timeline
├── screen/                         主菜单
│   ├── index.md                    主菜单钩子总览
│   ├── main-menu-hooks.md          MainMenuHooks（按钮 + 弹窗绑定）
│   └── replay-browser.md           ReplayBrowser（列表/筛选/排序/打开）
├── utils/                          工具层
│   ├── index.md
│   ├── path-utils.md
│   └── linked-hash-map.md
└── resources/                      资源与多语言
    ├── index.md
    └── ui-pack.md
```

## 阅读建议

- **5 分钟速通全貌**：[overview.md](overview.md)（一页 + 3 张图）
- **第一次接触**：先读本文件 → [overview/architecture.md](overview/architecture.md) → [playback/index.md](playback/index.md) → [playback/lifecycle.md](playback/lifecycle.md)
- **想理解录制**：[overview/record-flow.md](overview/record-flow.md) + [functions/record.md](functions/record.md) + [functions/action.md](functions/action.md) + [functions/io.md](functions/io.md)
- **想理解回放**：[overview/replay-flow.md](overview/replay-flow.md) + [functions/replay.md](functions/replay.md) + [functions/action.md](functions/action.md)
- **想理解 UI**：[overview/editor-flow.md](overview/editor-flow.md) + [editor/index.md](editor/index.md) → 各子模块
- **想理解主菜单入口**：[screen/index.md](screen/index.md) → [screen/replay-browser.md](screen/replay-browser.md)

## 与仓库其它部分的关系

- `README.md` / `README_ZH.md`：面向用户的安装/使用说明
- `xmake.lua`：构建配置，定义 `levilamina 26.10.*` 依赖和 `imgui dx12` 集成
- `manifest.json`：LeviLamina 入口声明
- `resources/`：UI 资源包（[resources/ui-pack.md](resources/ui-pack.md)）
- `CHANGELOG.md` / `CONTRIBUTING.md`：版本与协作流程
- `licenses/`：第三方依赖许可

## 关于"模块关系"

本文档不写"按步骤教你做"那种 install/dev 手册。每个模块文档的最后一节固定为"模块关系"，显式列出：

- **被谁调用**：上游触发方
- **调用谁**：下游依赖
- **共享数据**：通过事件 / 全局状态 / 单例共享的字段
- **事件订阅 / 发送**：LeviLamina 事件总线上的订阅关系

这样新人能直接看出"改这里会不会影响别处"。
