# 对话框背景虚化与弹出动效（DialogBackdropBlur）

## 目的

统一模态对话框的“背景虚化 + 弹出”表现，避免每个对话框各自复制一份遮罩实现。

## 位置

`qml/Main.qml` 内联组件：

- `component DialogBackdropBlur: Item` —— 虚化遮罩层
- `component DialogTextField: TextField` —— 对话框主题自适应输入框
- `component DialogSwitch: Switch` —— 开关行（替代复选框）
- `component DialogButton: Button` —— 对话框动作按钮

当前使用者：

- `serviceDialog`（添加服务）
- `privacyPinDialog`（隐私模式输入 PIN）

## DialogBackdropBlur

`Overlay.modal` 的共享实现。它把两个兄弟元素分别抓取、分别虚化：

| 抓取对象 | 覆盖范围 |
| --- | --- |
| `windowHeader` | 顶部工具栏，高度 = `windowHeader.height` |
| `mainPageRoot` | 工具栏以下的全部页面内容 |

两者都按 **1:1 几何**绘制（`MultiEffect` 的 `anchors` 与源的尺寸一致），因为
`ShaderEffectSource` 的纹理会被拉伸到效果项的尺寸：若只抓一个较小的项再铺满整屏，
得到的不是虚化而是被涂抹的放大图。历史上只抓 `windowHeader` 并 `anchors.fill` 全屏，
结果对话框四周出现一圈“光影”，看起来像边框变深，而不是背景虚化。

### 参数

- `dimOpacity`：黑色压暗层不透明度，由调用方按对话框的重要程度给定
  - `serviceDialog`：`darkTheme ? 0.35 : 0.16`
  - `privacyPinDialog`：`darkTheme ? 0.45 : 0.20`（门禁需要更强的阻断感）
- 虚化强度：`blurMax: 64`、`blur: 1.0`、`blurMultiplier: 0.8`
- 明暗分级：`brightness`、`saturation` 在浅色主题下明显收敛
  （`-0.3 / -0.15` 对比 `-0.1 / -0.05`），否则浅色页面会被压得发闷
- `autoPaddingEnabled: false`：保证与源元素严格对齐，不额外留白

`MultiEffect` 自带圆角无关的方形边缘，因此遮罩层不需要 `clip`；被虚化的内容始终位于
窗口内部，不会溢出到桌面。

## 弹出动效约定

两个对话框使用同一组过渡（`enter` / `exit` 为 `ParallelAnimation`）：

```qml
enter: Transition {
    ParallelAnimation {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.86; to: 1; duration: 260; easing.type: Easing.OutBack }
    }
}
exit: Transition {
    ParallelAnimation {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 110; easing.type: Easing.InCubic }
        NumberAnimation { property: "scale"; from: 1; to: 0.94; duration: 110; easing.type: Easing.InCubic }
    }
}
```

要点：

- 只动 `opacity` 和 `scale`。`Popup`/`Dialog` 用 `anchors.centerIn` 定位，**对 `x`/`y` 做
  动画会被锚布局覆盖**（需要位移时用 `Translate` 变换，参考服务表单的类型切换动画）。
- `OutBack` 提供轻微回弹，形成“从按钮弹出”的感觉；关闭用 `InCubic` 快速收走，不抢戏。
- 若某对话框需要更明确的“从控件甩出”，再单独定制，不要改动本约定，以免不同对话框动效漂移。

## 对话框配色

`serviceDialog` 的调色板按主题取值，浅色模式必须有浅色变体，否则白色控件会落在白色卡片上：

| 属性 | 深色 | 浅色 |
| --- | --- | --- |
| `dialogBg` | `#101114` | `#ffffff` |
| `dialogInput` | `#26282e` | `#f4f6fa` |
| `dialogBorder` | `#3a3d44` | `#dfe4ec` |
| `dialogText` | `#f4f7fb` | `#151922` |
| `dialogMuted` | `#9aa7b5` | `#5d6978` |
| `dialogHover` | `#31343c` | `#eaeef6` |

对话框内部的按钮不能复用全局 `ModernButton`：它使用 `theme.elevated`，浅色模式下是纯白，
落在浅色卡片上不可辨。改用 `DialogButton` / `DialogTextField` / `DialogSwitch`。

## 边界与注意事项

- **不要给对话框再加“假阴影”矩形**。此前 `background` 里有一块
  `anchors.margins: -14` 的纯黑矩形充当阴影，边缘无过渡，直接表现为贴边的一圈光影，已删除。
- 需要阴影时用带模糊的实现（例如再叠一层 `MultiEffect`），不要用纯色矩形。
- `mainPageRoot` 是页面内容根 `Rectangle` 的 id（原名无 id，为虚化新增）。重命名或调整窗口
  层级时需要同步 `DialogBackdropBlur` 内的两处 `sourceItem`。
- libmpv 通过窗口嵌入播放视频，其原生窗口不在 Qt Quick 场景图内，`ShaderEffectSource`
  抓不到视频画面；播放页打开对话框时视频区域会由遮罩的压暗层覆盖，这属于预期行为。
- 新增对话框若希望共享该观感，直接写 `Overlay.modal: DialogBackdropBlur { dimOpacity: ... }`，
  不要复制虚化参数。
