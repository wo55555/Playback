# 回放菜单生产实现

## 需求

提供全屏回放目录，统一编辑器配色，使用 TTF 图标，文本字号为 16 至 26 像素。支持搜索、排序、平铺和详情视图、打开、重命名、导入及二次确认删除。卡片为 4:3 容器，详情预览为 16:9 容器。

本轮补充需求：

- 回放名称降级策略：元数据名称缺失 / 为空 / 为默认占位符 `Unnamed` 时，使用文件名（不含扩展名）作为展示名称。
- 重命名（编辑）功能：点击「编辑」弹出名称输入框，同时修改归档内 `metadata.json` 的 `name` 字段与物理文件名，失败时回滚。
- 卡片右下角详细信息图标：透明背景，悬停时图标本身变白，并显示名称 / 世界 / 时长 / 大小 / 修改时间 / 文件名 / 路径等详细信息。
- 平铺 / 列表视图切换合并为单个按钮，点击即在两种模式间切换。
- 过滤、排序、导入、视图切换等文字按钮宽度自适应，按当前标签（含图标）计算宽度，避免文字被裁剪。
- 列表模式右半面板：预览图下方全部文字与按钮左对齐、距左边框 50px，标题字号最大（30px），元素上下间隔 2px，表格行内文字间隔 0.75px。
- 所在文件夹定位：使用绝对路径 + 资源管理器 `/select` 定位，失败时回退直接打开父目录。
- 卡片信息图标 tooltip 淡入：悬停瞬间透明度为 0，0.5s 后开始渐显，0.8s 时恢复为 1，规避首次悬停的短暂渲染异常。

## 架构

`ReplayBrowser` 负责解析、打开与重命名回放；`ReplayBrowserWindow` 仅维护展示状态、稳定回放 ID 选择、派生过滤索引和 ImGui 交互。平铺视图采用可见行裁剪，避免一次绘制所有卡片。详情视图复用同一选择状态和元数据源。缩略图解码和 GPU 上传由渲染器资源服务管理，缺失时保持主题封面。

重命名链路：

- `ReplayBrowser::renameReplay`：清洗新名称（过滤非法字符、去尾部 `.`/空格）→ 读取归档内 `metadata.json` → 修改 `name` → 通过 libzip `updateZipEntry` 写回 → 重命名物理文件为 `<name>.playback`；文件重命名失败时把原始元数据回写以回滚。
- 名称降级：`readReplaySummary` 先计算 `fileStem`，在 `metadata.json` 缺失 / 解析失败 / `name` 为空或 `Unnamed` 时以 `fileStem` 兜底；`displayName()` 保留同一逻辑供排序、搜索与展示复用。

## 执行

菜单打开时加载回放摘要，搜索或排序改变时重建索引，刷新后按 ID 保留有效选择。删除在确认窗口中执行并重新加载。重命名在模态对话框（`drawRenameDialog`）中执行，预填当前名称、支持回车提交，成功后刷新列表，失败展示错误弹窗。卡片右下角信息图标用透明 `InvisibleButton` + 手绘图标实现，悬停变色并弹出多行详情提示。导航栏宽度先按标签计算再布局，视图切换按钮常亮指示当前模式。验收包括全屏输入独占、排序后选择正确、名称降级生效、重命名后元数据与文件名同步、窄宽度详情可访问、海量回放仅绘制可见卡片、构建通过。

## 缩略图生成修复

### 需求

录制结束后 `.playback` 归档内必须包含 `icon.png`（640×360 RGBA），回放目录卡片与详情预览能显示缩略图；缺失时才显示占位提示。录制期间不得渲染 ImGui 覆盖层或清屏，避免干扰正常游玩画面。

### 架构

缩略图链路为「请求 → 捕获 → 保存 → 打包 → 读取 → GPU 上传」：

- `Recorder::start()` 在录制开始时请求捕获（`requestReplayThumbnailCapture`），`stop()` 时保存 `icon.png`。
- `ImGuiRenderer::render()` 在 Present 钩子中每帧执行；捕获分 D3D11（同步 CopyResource + 映射缩放）与 D3D12（命令列表 CopyResource + readback + fence 异步回读）两条路径。
- `ReplayExporter` 检测 `icon.png` 存在后写入 zip；浏览器读取 `thumbnailPng` 并经 `acquireReplayThumbnailTexture` 上传 GPU 供 `ImGui::Image` 使用。

### 执行

根因：`render()` 的门控 `!browserOpen && !state.replayVisible` 在录制期间（浏览器与回放均未打开，且录制时 `hudVisible` 恒为 false）必然命中提前返回，捕获代码永不执行，`thumbnailCaptureRequested` 恒为 true 但像素为空，`saveReplayThumbnail` 失败、无 `icon.png`。

修复：

1. 引入 `thumbnailActive = thumbnailCaptureRequested || thumbnailReadback != nullptr` 与 `uiActive = browserOpen || replayVisible`，`thumbnailActive` 时绕过两道 UI 门控；`!uiActive` 时关闭鼠标输入。
2. D3D11 `renderD3D11(sc, renderUi)` 增加 capture-only 分支：仅拷贝后台缓冲并捕获，不初始化 ImGui 帧、不渲染覆盖层、不清屏。
3. D3D12 路径以 `if (uiActive)` 包裹 ImGui 帧（NewFrame / draw / Render / 清屏 / RenderDrawData / 鼠标帧），保留命令列表中的 copy + readback 捕获章节与 present 状态转换；readback 在下一帧 `consumeD3D12Thumbnail` 消费，捕获完成后自动进入门控并 shutdown。
4. 显示侧补齐 D3D12 上传：`acquireReplayThumbnailTexture` 在无 `d3d11Device` 时走新增 `acquireD3D12ThumbnailTexture`——解码 PNG → upload heap 拷贝 → 命令列表 CopyTextureRegion → SRV 分配于 shader-visible `srvHeap` → Signal + 等待 fence 完成后返回 GPU 描述符句柄（`ImTextureRef` 按位透传，D3D12 后端直接使用）。`clearReplayThumbnailTextures` 与 `shutdown` 同步释放 SRV / 清空缓存，避免句柄悬挂。

第二轮修复（生成后文件为空 / 打不开）：用独立测试程序复现 WIC 编码，确认 PNG 编码器在 `SetPixelFormat(GUID_WICPixelFormat32bppRGBA)` 时会把请求格式**改写**为 `32bppBGRA`，导致旧代码的严格校验 `format != 32bppRGBA` 直接返回 false——此时文件已被 `InitializeFromFilename` 创建但内容为空。同时直接 `WritePixels` 会按改写后的格式解释数据，产生 R/B 通道错乱。修复：

- `writeReplayThumbnailPng` 改为 `CreateBitmapFromMemory(32bppRGBA)` 包装像素 + `frame->WriteSource(bitmap)` 写入，格式转换由 WIC 自动完成（测试验证颜色与原数据逐字节一致）。
- 新增 `ComInitialize` RAII 兜底：`CoInitializeEx` 仅在返回 `S_OK`（本线程首次初始化）时在函数结束时 `CoUninitialize`，`S_FALSE` / `RPC_E_CHANGED_MODE` 视为已初始化，保证 WIC 在未初始化 COM 的辅助线程也可用。
- `decodeReplayThumbnailPng` 同步加 `ComInitialize` 兜底。

验收：录制任意时长的回放后归档内含 `icon.png`（可被图片查看器打开，且可由 WIC 解码回读，颜色无通道错乱）；D3D11 与 D3D12 下卡片 / 预览均可显示缩略图；录制与播放画面不受影响；`xmake build` 通过。

第三轮修复（悬停详情提示框永不出现）：`mInfoHoverStart` 是窗口类的**单个成员变量**，被所有卡片共享。悬停某卡片图标时，同一帧内**在它之后绘制的其它卡片**的 `else` 分支每帧把该成员重置为 `-1.0`，导致悬停卡片下一帧重新记录起始时间，`elapsed` 恒为 0、`tooltipAlpha` 恒为 0——图标（局部 `infoHovered` 驱动）正常变白，但 tooltip 永远透明。修复：改用 `ImGui::GetStateStorage()` + `ImGui::GetID("##info-fade")`（`PushID(replayId)` 已隔离各卡片）保存每张卡片的悬停起始时间，互不干扰；删除共享成员。验收：悬停图标 0.5s 后提示框渐显、0.8s 完全可见，多卡片并存时行为一致，`xmake build` 通过。
