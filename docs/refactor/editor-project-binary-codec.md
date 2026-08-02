# 编辑工程二进制编解码

## 需求

- `editor.bin` 是 v3 编辑工程的唯一完整状态载体。
- 编解码保存 Sequence、WorldActor、SubActor、Camera、关键帧与 Marker。
- 文件必须包含魔数、格式版本和 CRC32 校验；无效数据不得产生部分状态。

## 架构

- `EditorProjectCodec` 是纯内存编解码器，不依赖 ImGui、ReplaySession 或 ZIP。
- 格式以固定宽度标量、小端序、长度前缀字符串和集合数量构成。
- 解码过程限制字符串和集合规模，并在 CRC、魔数、版本、枚举或边界不合法时失败。

## 执行

- `encode` 将 v3 权威领域数据写入 `editor.bin` payload，并追加 CRC32（IEEE）尾部。
- `decode` 先校验完整性和格式头，再构建完整 `EditorStateExt`；失败时返回空值。
- 模型测试覆盖 round-trip 与篡改数据拒绝；后续归档层负责把该字节流写入 ZIP 并进行原子替换。
