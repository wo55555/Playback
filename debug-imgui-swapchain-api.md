# ImGui SwapChain API 调试记录

状态：[OPEN]
会话：`imgui-swapchain-api`

## 现象

ImGui 完全不显示。Present Hook 已命中，但无法取得 D3D12 Device 和 Direct Command Queue。

## 现有证据

- RendererInitHook 已触发。
- 核心与捕获 Hook 均安装成功。
- CreateSwapChainForHwnd 已触发，但传入对象无法转换为 ID3D12CommandQueue。
- Present SwapChain 的 GetDevice(ID3D12Device) 返回 0x80004002（E_NOINTERFACE）。
- replayVisible=1，但 hudVisible=0。

## 假设

1. 实际 SwapChain 属于 D3D11，而不是 D3D12。
2. CreateSwapChainForHwnd 的 device 参数是 D3D11 Device 或其他 DXGI 对象，而不是 D3D12 Queue。
3. 游戏使用 D3D11-on-12，真实 D3D12 Queue 被包装层隐藏。
4. 当前 Hook 命中了辅助 SwapChain，而非游戏主画面。
5. hudVisible 门控是第二个独立问题。

## 观测计划

- 在 CreateSwapChainForHwnd 中探测传入对象支持的 D3D12 Queue、D3D12 Device、D3D11 Device 和 IDXGIDevice 接口。
- 在 Present 中探测 SwapChain 的 D3D12 Device、D3D11 Device、IDXGIDevice、尺寸、格式和窗口句柄。
- 保留现有可见性日志。

## 证据判定

- 假设 1 确认：CreateSwapChain 和 Present 均能取得 `ID3D11Device`，不能取得 `ID3D12Device`。
- 假设 2 确认：CreateSwapChainForHwnd 的 device 参数就是 D3D11 Device。
- 假设 3 否定：当前对象没有暴露 D3D12 Device/Queue，暂无 D3D11-on-12 证据。
- 假设 4 否定：Create 与 Present 的尺寸、格式和窗口一致，命中的是 1920x1009、R8G8B8A8 主窗口 SwapChain。
- 假设 5 确认：回放可见后 `hudVisible` 仍为 false，是 Queue 问题之外的第二个阻断点。

## 根因

项目实际运行于 D3D11，但编辑器渲染链完整实现为 D3D12。D3D12 Queue 捕获、资源屏障和 `ImGui_ImplDX12` 后端均不适用于当前 SwapChain，因此 ImGui 不可能提交。即使切换后端，当前 HUD 判定仍会阻止绘制。

## 下一步

采用 D3D11 原生渲染路径，并修正回放编辑器的 HUD 可见性门控；保留 D3D12 代码但根据 SwapChain Device 类型选择后端。
