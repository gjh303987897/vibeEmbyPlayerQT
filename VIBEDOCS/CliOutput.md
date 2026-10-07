# CLI 输出格式模块

## 用途与边界

`src/cli/CliOutput.h/.cpp` 把命令的 `Invocation`、`Result` 转成终端文本。
独立命令与交互 Shell 共用此模块。不读取配置、数据库或凭据，不调用网络，
不改变任务结果或退出码，也不引入 Qt GUI 依赖。

```cpp
Cli::RenderedOutput Cli::renderResult(
    const Invocation& invocation, const Result& result, int columns = 120);
```

返回 `standardOutput` 与 `standardError` 两段文本。`CliCommand::writeResult`
负责用 UTF-8 分别写 stdout/stderr；`CliConsole::outputColumns` 只查询终端宽度。
因此可以在单元测试中直接检查输出，不需要替换进程的标准流。

## 人工阅读格式

- 列表带名称、数量、列标题与分隔线，使用空格对齐，不依赖 Tab 的显示宽度。
- 服务列表先显示名称、类型、已保存会话/凭据；每项下方显示完整 ID 与地址。
  `Saved` 表示本地保存过认证信息，`None` 表示没有，均不表示在线状态。
- TSSL 列表显示文件名、大小、修改时间，每项下方显示完整摘要和文件路径。
- WebDAV 的 `directory` 显示为 `Folder` / `File`。文件大小使用 B、KiB、MiB、
  GiB、TiB；目录大小或未知大小显示 `-`。JSON 中保留原始布尔值和字节数。
- 对象使用对齐的 `字段 : 值`；配置键保留原文，可直接用于 `config get/set`。
- 批量结果先显示成功/失败等计数，再显示 `Items (数量)` 明细表。
  列取所有条目的字段并集，后续失败条目的 `error` 不会因首项成功而丢失。
- 错误在 stderr 显示命令、退出码、错误类别与说明；部分失败的已完成项仍在
  stdout 中。日志和进度沿用原有 stderr 路径。
- 帮助按服务、WebDAV、TSSL、视频、配置、Shell 分组，并说明全局参数和示例。

例如：

```text
Services (2)

Name      Type    Saved session  Saved credentials
--------  ------  -------------  -----------------
家庭影片  WebDAV  None           Saved
  ID      : <完整服务 ID>
  Address : https://example.com/dav/

Home      Emby    Saved          None
  ID      : <完整服务 ID>
  Address : https://example.com/emby/
```

## 宽度、Unicode 与重定向

Windows 使用 `GetConsoleScreenBufferInfo` 的可见窗口宽度；POSIX 使用 stdout
的 `TIOCGWINSZ`。stdout 重定向或查询失败时固定采用 120 列，不读取输入控制台
的宽度。布局宽度限制为 20–240 列。

表格根据内容计算列宽，必要时压缩并换行；列标题无法容纳时，改为逐条字段
展示。长 ID、摘要与路径完整保留，通过续行展示，**不省略或截短值**。
在需要机器读取或复制长路径时使用 `--json`。

Unicode 通过 Qt 的 Grapheme 边界换行，避免拆开代理对和组合字符。中文及
常用全角字符按两个终端单元计算，组合附加字符不单独占列，并兼容常见 Emoji。
复杂文字、特殊字体和不同终端的 Emoji 宽度仍可能存在显示差异。

外部字符串中的控制字符、换行和双向格式控制字符转换为空格，防止字段内容
插入终端控制序列或伪造新行。人工输出没有 ANSI 颜色码，重定向可直接阅读。
错误详情继续复用 `ErrorPresentation::sanitizeDetails` 隐去敏感信息。

## JSON 兼容

`--json` 保持首版协议，每条命令在 stdout 中只写一行完整对象：

```json
{"schemaVersion":1,"command":"webdav stat","ok":true,"data":{"directory":false,"name":"影片.mkv","size":1572864},"error":null}
```

JSON 不应用列标题映射、单位换算、宽度换行或控制字符替换，保留原始字段、
数据类型与完整内容；错误/消息继续使用现有敏感信息清理。失败仍包含原来的
`error.code/message`，stderr 中保留原来的错误说明。

## 验证与实现依据

`tests/CliTest.cpp` 验证中文和英文列位置、完整 ID/长地址续行、文件/目录及大小、
批量字段并集、窄窗口、Unicode 完整性、控制字符和 JSON 协议。
`cli_integration` 继续验证命令退出码、实际任务和 JSON 标准流分离。

实现依据：[Qt QTextBoundaryFinder](https://doc.qt.io/qt-6/qtextboundaryfinder.html)、
[Windows GetConsoleScreenBufferInfo](https://learn.microsoft.com/en-us/windows/console/getconsolescreenbufferinfo)。
