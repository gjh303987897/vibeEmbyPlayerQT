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

`TrayController` owns platform tray integration through `QSystemTrayIcon`.

## Tray Behavior

Current behavior:

- Show tray icon when minimize-to-tray is enabled and the platform supports a tray.
- Hide main window via QML action or close event.
- Restore main window from tray activation or tray menu.
- Quit from tray menu.
- If the platform does not report tray availability, window close is allowed to quit normally instead of hiding an unreachable window.

## Settings

`SessionRepository` stores desktop settings through `QSettings`.

Current setting:

- `desktop/minimizeToTray`

## Platform Notes

- Windows tray behavior should be verified directly.
- macOS may present the icon in the menu bar area and still has Dock behavior to validate.
- Linux tray support varies by desktop environment and should be validated on at least one GNOME/KDE-style setup.
