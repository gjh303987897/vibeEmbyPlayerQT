# ThemeTransition — 明暗主题切换动效

## 概述与交互时序

用户在设置页点击「深色 / 浅色」分段控件后，动效分两拍：

1. **按钮先动**（0–200ms）：OptionSegmentedControl 选中块滑移；
2. **页面溶解变色**（约 300ms 起，再 900ms 匀速）：旧画面以截屏快照形式淡出，露出已换成目标主题的实时 UI——视觉上所有组件都在向目标颜色平滑渐变。快照就绪后是**先盖快照、等 50ms 确认已显示、再换色**（flip），绝不允许换色与盖快照同帧。

实现：`qml/Main.qml`（时序与溶解层）+ `src/app/ThemeSnapshotController.{h,cpp}`（截屏）。

## 架构

```
onDarkThemeChanged ─(启动期/未armed→瞬时应用)
  └─ themeMorphKickoff (300ms)         // 等按钮滑移(200ms)先播完
       └─ beginThemeDissolve()
            ├─ themeSnapshotController.grabContent(root.contentItem)   // C++
            │    QQuickWindow::grabWindow() 场景图重渲染 → 临时 JPEG → ready(token, url)
            ├─ onReady: pendingReveal=true; snapshot.source=url
            └─ Image.onStatusChanged Ready (枚举注意: Null=0, Ready=1, Loading=2, Error=3):
                 layer.opacity=1 盖住旧画面（旧UI+旧快照同构，无感）
                 → themePaletteFlipTimer(50ms): themeApplyPalette(target)  // 快照下换色
                 → fade 900ms Linear 快照匀速淡出
```

- `themeDissolveLayer`：`Overlay.overlay` 内的常驻可见覆盖层（**opacity 0 待机，绝不用 visible**，见坑 3），底色 Rectangle（新底色兜底快照透明像素）+ 快照 Image。动画目标是整层 opacity。
- `ThemeSnapshotController`：context property `themeSnapshotController`；`grabContent(item)` 同步 `QQuickWindow::grabWindow()`（场景图重渲染，含纹理）存 `%TEMP%/vibeplayer-theme-snapshot-<token>.jpg`（JPEG q=90），下一事件循环 emit `ready(token,url)` / `failed(token,reason)`。token 防串扰；旧快照文件保留一代再删（正在溶解的快照文件不可删）。另有 `log(msg)` 遥测入口（写入 area=`theme` 日志，排查时用 `VIBEPLAYER_LOG_FILE` 开启落盘）。
- 三层兜底全部收敛到 `themeApplyPalette(target)` 瞬时换色：failed（抓取/落盘失败）、themeDissolveFallback（1200ms）、themeRevealFallback（解码卡死 1200ms）。主题切换不可能丢失。
- 中途连续切换：`themeAbortDissolve()` 清 pendingGrabToken/Target 并作废旧 token 的结果，从当前画面重新开始。
- 启动恢复主题不播动画：`themeMorphArmed`（800ms 后 arm）。

## 踩过的坑（全部有实测证据，勿回退）

1. **QML `Item.grabToImage()` 不能用于 `contentItem`/`Overlay.overlay`**：它们没有 QML 创建上下文，直接失败（`grabToImage: item has no QML engine`），导致早期所有切换走瞬时兜底（表现为"延迟一下然后瞬间换色"）。
2. **抓屏必须用场景图 `QQuickWindow::grabWindow()`，绝不能用 `QScreen::grabWindow(winId)`**：平台 BitBlt 读回在 DWM DirectFlip 扫描输出下（无边框/最大化/无遮挡窗口）返回**全黑图**——实测：强切白色主题后 grabWindow(winId) luma=32（纯黑）。而当时“黑图”曾被误归因为“grabToImage 离屏重渲染丢纹理”，其实丢纹理指控不成立：现方案 `QQuickWindow::grabWindow()` 同步场景图抓取内容完整（白屏 luma=241，海报墙纹理完好），且不受 DWM 演示模式影响，窗口被遮挡/出屏也能拍到完整内容。同步重渲染一次性阻塞约 10-30ms，处在按钮动画后的空档，无感。
3. **`Image` 在 `visible:false` 的父项下不解码**：快照永远不到 `Ready`。覆盖层因此常驻可见、用 `opacity: 0` 待机（opacity 0 不产生像素、不拦截鼠标、不会被自己的 grab 拍进去——grabWindow 拍的是真实屏幕，opacity 0 时本来就无内容）。
4. **`Connections.onReady` 里必须先 `pendingReveal = true` 再赋 source**（rewire 时丢过一次，表现为“抓到了快照但画面永远不变色”）。
5. id 不是 root 的属性：JS 里写 `root.themeDissolveFallback` 是 undefined，直接写 `themeDissolveFallback`。
6. **换色与盖快照同帧会漏白帧**（dark→light 方向尤其明显）：快照的大纹理首帧上传赶不上换色后那一次渲染，会先闪 1-2 帧新主题再盖上旧快照，用户解读为“白闪 + 按钮动效重播”。必须 cover→确认显示（50ms）→flip→fade 分拍执行。
7. **Image 状态枚举序是 Null=0, Ready=1, Loading=2, Error=3**（不是 Loading=1）；读错会把正常遥测误判成断链。
8. DWM **DirectFlip**（无边框/最大化/无遮挡窗口直接扫描输出）会让**所有基于 BitBlt 的读回（GDI 桌面连拍、`QScreen::grabWindow`）抓不到窗口内容甚至全黑**：自证截图不可靠，应用内日志/抓帧遥测才是真相源；应用内抓屏必须走场景图（见坑 2）。曾因此把“黑快照盖屏”当成设计目标去调参，黑→白黑洞、白→黑硬切两个方向的“突兀”全部源于此。
9. **DWM 原生窗口件（IMMERSIVE_DARK_MODE 标题栏、CAPTION_COLOR、圆角抗锯齿）画在 QML 场景之上，快照盖不住它们**：applyTheme 过去在点击瞬间瞬时切换，标题条先变色、页面 450ms 后才追上，即“按钮动效又演了一遍 + 突兀”的真相（dark→light 敏感，反向无感）。现已把 applyTheme 搬进 themeApplyPalette()，与色板同帧（快照底下）切换；日志顺序“captured → native-applyTheme → fade”即正确时序。副作用：启动时 themeApplyPalette 必须在 windowAppearanceController.attachWindow 之后调用。
10. **驱动硬编码明暗表达式的 bool（`darkTheme ? … : …`，Main.qml 内 119 处）同样必须延迟翻转**：它直接绑 VM 时点击当帧就变色，对话框背景/遮罩/芯片底色全部抢在快照前换装。现 `vmDarkTheme`（VM 镜像，立即）与 `darkTheme`（视觉值，仅 themeApplyPalette 内翻转）分离；dark/light 色板对象携带 `dark` 字段，themeApplyPalette 首行 `darkTheme = p.dark` 成为全应用唯一换色入口（也顺带断开属性初始化器绑定）。VM 驱动的分支逻辑用 vmDarkTheme，视觉呈现用 darkTheme。

## 关键参数

| 属性 | 当前值 | 说明 |
| --- | --- | --- |
| `root.themeMorphDelayMs` | 300 | 按钮动效与页面溶解的衔接；须 ≥ 滑移 200ms |
| `root.themeMorphDuration` | 900 | 溶解时长；**Linear 匀速是刻意的**（InOut 缓动前段与屏幕无差异，体感永远"快"） |
| `root.themeMorphArmed` | 启动 800ms 后 true | 冷启动恢复主题不播动画 |
| `ThemeSnapshotController` | src/app | 快照抓取；日志 area=`theme`（`VIBEPLAYER_LOG_FILE` 环境变量开启落盘） |

## 验证方法（无 UI 自动化下的自证）

首选**应用内抓帧遥测**：临时自动两段切换（先白→黑）+ fade 期间每 ~120ms 调 `themeSnapshotController.grabContent()` 存真实像素帧，并在 grabContent 内加临时**平均亮度探针**（采样步远 16px）+ `layer.opacity` 轮询，`VIBEPLAYER_LOG_FILE` 落盘后读日志：溶解正常时 luma 轨迹应严格线性（实测黑→白 32→61→90→118→145→175→200，白→黑 241→211→175→141→108→77→46）；若全程恒定则是快照/覆盖层本身坏了。另可用“强制换色后 grab”作为判别实验区分“页面没变色”与“抓屏抓黑”。桌面级 CopyFromScreen 连拍受“窗口未前置/DirectFlip”干扰，结论不可靠。完成后删除临时代码。

## 已知限制

- 场景图抓取与屏幕像素无关：多显示器/遮挡/圆角外圈都不影响快照内容；溶解层圆角外区域由 layer 底色补齐，不可见。
- 正在播放时切主题：mpv 是独立原生子窗口，场景图抓取不含其画面（快照中为黑区）；播放中切主题频率低，可接受，必要时后续对播放页改走瞬时切换。
- 溶解期间（900ms）底层 UI 的新主题悬停反馈被快照遮挡，结束后自然恢复。
- 弹窗内写死的 `darkTheme ? "#…" : "#…"` 局部调色在换色帧瞬时切换，被快照遮挡不可见。
