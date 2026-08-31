# 分段选择控件（OptionSegmentedControl）

## 目的

为"少量、固定、需要横向比较"的枚举值提供统一的按钮组选择控件，替代
`ModernComboBox` 下拉框：所有候选项一次可见，切换时选中块整体滑移到新项，
而不是重新绘制一行。

## 位置

`qml/Main.qml` 内联组件 `component OptionSegmentedControl: Rectangle`。

## 接口

| 成员 | 说明 |
| --- | --- |
| `options` | `{ label, value }` 数组，`label` 为显示文案，`value` 为业务值 |
| `selectedValue` | 当前选中值；由外部绑定，改变它即改变选中项 |
| `chosen(string value)` | 用户点击某一段时发出；组件自身不改状态，写回由使用方决定 |
| `selectedTextColor` | 选中块上的文字色（只读，`#ffffff`） |

使用示例（设置页"主题"）：

```qml
SettingRow {
    label: t("settings.theme")
    OptionSegmentedControl {
        options: [
            { label: t("option.system"), value: "system" },
            { label: t("option.dark"),   value: "dark" },
            { label: t("option.light"),  value: "light" }
        ]
        selectedValue: appViewModel.themeMode
        onChosen: appViewModel.themeMode = value
    }
}
```

## 设计规则

- **一个滑块，不是每项各自高亮。** 选中态由 `segmentedThumb` 单个矩形表达，
  它跟随 `segmentedRow.selectedItem`（最后被选中的那段），只动画 `x`
  （200ms `OutCubic`）。各段几何相同，所以滑动不伴随尺寸变化。
- **滑块声明在分段行之前**，因此绘制在文字下方；每段自身背景静止时必须透明，
  否则会把滑块盖住。
- **等宽分段。** `Layout.fillWidth` 单独使用时隐式文字宽度会主导分配，实测
  `跟随系统 / 深色 / 浅色` 会分成 98 / 55 / 55，滑块滑动时还要变形。因此显式给
  `Layout.preferredWidth = (行宽 - 间距总和) / 段数`，实测得到 69 / 70 / 69。
- **悬停用 opacity 动画，不用 color 动画。** 见 `SettingsAppearance.md` 的 Theme
  一节：`"transparent"` 是 `#00000000`，`ColorAnimation` 逐通道插值会先压暗再回亮，
  表现为闪动。这里以不透明的 `theme.elevatedHover` 铺底、用 `opacity` 0↔1 淡入淡出，
  既能让出静止透明又不产生黑色中间态。
- **文字色可以用 color 动画**：`theme.text ↔ #ffffff` 两端都不透明，插值安全。
- 分段点击后通过 `chosen` 通知外部，组件不持有状态，保持与 `appViewModel`
  单一数据源一致（QML 不写 `QSettings`）。

## 当前使用者

- 设置页 → 外观 → 主题（`system` / `dark` / `light`）

## 后续可收敛

`HomeLayoutSelector`（Emby / Jellyfin 首页布局、播放器布局两选一）是同一类控件的
早期硬编码版本：两段各写一遍、无滑动动画、悬停无过渡。需要时可用它替换，
把 `selectedLayout` / `layoutChosen` 映射到 `selectedValue` / `chosen`。
