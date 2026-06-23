# WindowAppearanceController

## 模块职责

`WindowAppearanceController` 位于 `src/app`，负责处理主窗口原生标题栏的外观适配。它不接管窗口生命周期，也不替换系统标题栏，只针对平台允许的外观属性做轻量调整。

## 当前实现

- QML 根窗口通过 `attachWindow(QObject*)` 传入 `ApplicationWindow`。
- QML 在主题变化时调用 `applyTheme(QString)`。
- Windows 平台使用 DWM API 设置标题栏深色模式、标题栏背景色、边框颜色和标题文字颜色。
- 非 Windows 平台当前为 no-op。

## 设计边界

- 保留系统原生最小化、最大化、关闭按钮。
- 保留系统原生窗口拖拽、缩放、吸附等行为。
- 不在 QML 层实现窗口控制业务逻辑。
- 后续如果需要完全自绘标题栏，应在评估跨平台窗口拖拽、缩放和播放器原生子窗口叠加后再单独设计。

## 相关文件

- `src/app/WindowAppearanceController.h`
- `src/app/WindowAppearanceController.cpp`
- `src/app/main.cpp`
- `qml/Main.qml`
