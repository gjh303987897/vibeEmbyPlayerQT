# Desktop Lifecycle

## Scope

First version targets desktop only:

- macOS
- Windows
- Linux

TV and remote-control focus behavior are out of scope.

## Window Behavior

The QML `ApplicationWindow` handles close events:

- If minimize-to-tray is enabled, closing the window hides it to tray.
- If disabled, the application can exit normally.

On Windows the same close path is invoked by the application-drawn title-bar
button. It calls `ApplicationWindow.close()` rather than terminating the
process, so minimize-to-tray remains authoritative.

`TrayController` owns platform tray integration through `QSystemTrayIcon`.

## Tray Behavior

Current behavior:

- Show tray icon when minimize-to-tray is enabled and the platform supports a tray.
- Hide main window via QML action or close event.
- Restore main window from tray activation or tray menu.
- Quit from tray menu.
- If the platform does not report tray availability, window close is allowed to quit normally instead of hiding an unreachable window.
- Window and tray surfaces always use the light-theme application icon (`icon_black.png`). Theme changes affect the interface palette only and do not replace the application icon. Startup applies the icon both before QML loading and after the root native window exists so Windows receives a concrete taskbar `HICON` instead of relying only on the application default.

## Settings

`SessionRepository` stores desktop settings through `QSettings`.

Current setting:

- `desktop/minimizeToTray`

## Platform Notes

- Windows tray behavior should be verified directly.
- macOS may present the icon in the menu bar area and still has Dock behavior to validate.
- Linux tray support varies by desktop environment and should be validated on at least one GNOME/KDE-style setup.

## Windows graphics startup diagnostics

For local builds, `scripts/run-desktop.ps1` provides Qt/OpenSSL runtime paths
without changing persistent environment settings. Hardware graphics remain the
default. `-SoftwareRendering` opts into Qt's D3D11 WARP interface renderer for
machines whose graphics driver crashes during startup. It preserves RHI effects
and leaves the separately embedded libmpv renderer unchanged.

The 2026-10-08 startup crash was reproduced in Intel driver
`igd10iumd64.dll` (`27.20.100.9565`, offset `0x291660`, invalid read at `0xE8`).
WARP passed a 90-second native debug-event observation without an access
violation. See [Windows build notes](../docs/BuildWindows.md#intel-d3d11-startup-crash-workaround)
for the launch command, limitations and the official Qt graphics reference.

## Modal backdrop ownership

`DialogBackdropBlur` assigns an explicit visual parent to each
`ShaderEffectSource` created inside a `MultiEffect.source` property. Captures
clear `sourceItem` while the backdrop is hidden. In Qt 6.7.3, parentless captures
can retain scene/window references after the popup closes; the IPTV playback,
service-popup and picker regression previously crashed during root-window focus
cleanup. The corrected ownership is covered by `native_iptv_import_playback`.
See [file-selection lifecycle](FileSelection.md) for details and verification.
