# Config

`Config` 是 mod 的静态配置，定义在 `src/playback/Config.h`。当前没有从磁盘加载的机制，所有值都是头文件里硬编码的默认值。`Playback::getConfig()` 暴露为常驻引用，可被下游模块读取（如 `command::registerRecordCommand` 读 `command.record.enabled` / `command.record.command`）。

## 字段一览

```cpp
struct CommandConfigStruct {
    bool        enabled;
    std::string command;
};

struct CommandStruct {
    CommandConfigStruct record = {true, "record"};
};

struct Config {
    int         version    = 1;
    std::string locateName = "zh_CN";

    CommandStruct command;
};
```

| 字段 | 类型 | 默认 | 含义 |
| --- | --- | --- | --- |
| `version` | `int` | `1` | 配置 schema 版本。当前未用作迁移入口，留作后续扩展。 |
| `locateName` | `std::string` | `"zh_CN"` | UI 默认语言键。当前**没有**被代码读取；UI 通过 `resources/texts/languages.json` 决定默认语言。 |
| `command.record.enabled` | `bool` | `true` | 是否注册 `record` 命令族。`false` 时 `registerRecordCommand` 直接 return。 |
| `command.record.command` | `std::string` | `"record"` | `record` 命令的命令名，可改成其它（如 `"rec"`）以避免冲突。 |

## 用法

- 读取：`Playback::getInstance().getConfig().command.record`。
- 修改：当前没有持久化层，构造期改默认值或者在 `Playback::load()` 里读磁盘（待实现）。

## 与 `playback` 之外的关系

- `command/Record.cpp` 读 `command.record.enabled` / `command.record.command`。
- `i18n` 通过 `Playback::getInstance().getSelf().getLangDir()` 加载 `src/lang/*.json`，与 `Config` 无关。
- UI 资源包通过 `resources/texts/languages.json` 暴露可用语言列表。

## 扩展点

- 加新命令族：在 `Config` 加 `CommandConfigStruct xxx = {true, "xxx"};` 字段，在 `CommandStruct` 暴露，然后在 `command/*.cpp` 写 `registerXxxCommand(config.command.xxx)`，在 `Playback::setupCommands()` 调用。
- 改 schema：递增 `version`，并在加载逻辑里做迁移。
