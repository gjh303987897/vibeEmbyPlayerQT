# WindowAppearanceController

## 模块职责

`WindowAppearanceController` 位于 `src/app`，负责连接 Qt Quick 主窗口与平台窗口管理能力。它不接管窗口生命周期；关闭事件和托盘策略仍由现有 QML 与 `TrayController` 负责。

## 当前实现

- QML 根窗口通过 `attachWindow(QObject*)` 传入 `ApplicationWindow`。
- QML 在主题变化时调用 `applyTheme(QString)`。
- Windows 主窗口使用 `Qt.FramelessWindowHint` 隐藏系统标题栏，不再保留独立的应用顶部状态条。最小化、最大化/还原和关闭按钮位于应用工具栏最右侧，与设置、添加和编辑等页面操作共用一栏。
- 服务页在窄窗口中将保号任务和历史统计收进“更多”菜单，优先保证添加、编辑、设置及窗口按钮始终完整可见；宽窗口继续直接展示全部操作。
- 应用工具栏通过不参与布局的 `DragHandler` 识别拖动手势，再由 `startSystemMove()` 调用 `QWindow::startSystemMove()`。因此标题和按钮之间的空白区域都可移动窗口，普通点击仍由业务按钮处理。八个边缘/角落通过 `startSystemResize(Qt::Edges)` 交回系统处理，因此保留 Windows 吸附、跨屏拖动和原生缩放约束。
- 双击应用工具栏的页面标题区域切换最大化与还原；最大化和全屏时禁用边缘缩放热区。
- 无边框模式仍应用 Windows DWM 深色窗口属性与边框颜色，但跳过已不存在的原生标题文字和标题栏背景设置。
- 所有平台都会请求 Qt Quick 窗口和内容场景重绘；非 Windows 平台不调用原生标题栏 API。

## 设计边界

- 无边框窗口及内置窗口按钮当前只在 Windows 启用；macOS 与 Linux 继续使用各平台原生窗口框架。
- QML 只负责按钮呈现和交互，系统移动/缩放入口由 C++ 控制器提供。
- 内部关闭按钮调用 `ApplicationWindow.close()`，因此不会绕过最小化到托盘策略。
- 播放器沉浸/全屏状态隐藏应用工具栏，避免覆盖视频表面和播放器控制层。

## 验证

- Windows 125% 缩放下验证了应用工具栏布局、最大化、还原和最小化。
- 最大化前后窗口从 `1550x975` 进入 `2560x1380` 工作区，并能还原到原始位置和大小。
- 相关 Qt 接口：`QWindow::startSystemMove()`、`QWindow::startSystemResize()` 与 `Qt::FramelessWindowHint`。

## 相关文件

- `src/app/WindowAppearanceController.h`
- `src/app/WindowAppearanceController.cpp`
- `src/app/main.cpp`
- `qml/Main.qml`
