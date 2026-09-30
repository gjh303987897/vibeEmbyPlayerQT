# File and folder selection

## Shared controller

`FileDialogController` owns all local filesystem pickers. QML asks
`AppViewModel` to start an operation; the ViewModel supplies a translated
caption, initial path, filters and a continuation. The controller returns
local paths after selection, and the existing services validate them before
performing playback, import, export or transfer work.

The application UI remains Qt Quick. `QFileDialog` is used only as an adapter
to Qt's native dialog support; Qt Widgets was already a project dependency.
No picker uses static `getOpenFileName`, `getSaveFileName`,
`getExistingDirectory` or a nested `exec()` event loop.

## Native preference and compatibility

| Operation | Backend policy |
| --- | --- |
| Open one file | Native when the platform supports it |
| Open multiple files | Native when the platform supports it |
| Save a file | Native; default suffix and overwrite confirmation handled by Qt |
| Select one folder | Native directory picker with `ShowDirsOnly` |
| Select multiple source folders | Qt widget fallback with extended directory selection |

Qt's public `QFileDialog::FileMode` provides `Directory` for one folder and
`ExistingFiles` for multiple files, with no multiple-directory mode. The
M3U8S source picker keeps the existing ability to choose several folders in
one operation. This is the only explicit `DontUseNativeDialog` exception;
ordinary operations let Qt choose a native implementation or its automatic
fallback on Windows, macOS and Linux.

Local pickers cover subtitles, IPTV playlists, local media roots, WebDAV
uploads and download destinations, TSSL import/export and backup paths,
M3U8S source videos/folders, FFmpeg, and packaging output/fallback/temporary
directories. Browsing folders **on a remote WebDAV server** remains in the
application because those URLs are not local filesystem folders.

## Ownership and asynchronous lifecycle

- The root Quick window is attached before any picker can be opened.
- The controller allocates the dialog on the heap and keeps it alive until
  `finished`. Its native window has the Quick window as transient parent,
  uses `Qt::WindowModal`, and has `WA_QuitOnClose` disabled.
- `QDialog::open()` returns immediately. In Qt 6.7.3's Windows backend, this
  route lets Qt start its own STA Shell-dialog thread. No worker thread in
  application code constructs a GUI object or invokes Windows Shell APIs.
- Only one picker may be active. Repeated requests leave the existing picker
  and its continuation intact.
- Cancel returns an empty path/list. Navigation, server changes, owner hiding
  and owner destruction cancel the active picker. Controller destruction
  disconnects completion and never runs business callbacks.
- Ownership is cleared before invoking a continuation, so it can navigate
  or open another picker safely. The finished dialog uses `deleteLater()`.
- URLs are restricted to the local `file` scheme. Logs record operation
  outcome and selected count, without file paths or credentials.

ViewModel continuations capture item, digest and service values rather than
row numbers or pointers into a changing model. WebDAV upload also verifies
that the chosen service and target directory still match. Subtitle selection
checks the playback URL before applying a file to the current player.
Save results are used unchanged: `defaultSuffix` supplies `.tssl` when needed,
so overwrite confirmation and the actual export refer to the same path.

## Verification

`file_dialog_controller_test` exercises all modes, window ownership,
asynchronous timer delivery, duplicate requests, acceptance/cancellation,
multiple file/folder selection, save suffixes, reentrant continuations,
owner hiding/destruction and controller teardown without business callbacks.
It operates only on temporary paths and does not upload, import or export
user data.

CTest uses the offscreen platform and explicitly locates it through the
installed `Qt6::QOffscreenIntegrationPlugin` target. The application deploys
the normal desktop platform plugin; the offscreen plugin is needed only for
this test. These tests verify the shared controller and Qt fallback behavior,
not the visual appearance of each operating system's native picker.

## Official references

- [QFileDialog modes, options and native defaults](https://doc.qt.io/qt-6/qfiledialog.html)
- [QDialog asynchronous open and lifetime](https://doc.qt.io/qt-6/qdialog.html#open)
- [QWindow transient parent](https://doc.qt.io/qt-6/qwindow.html#transientParent-prop)
- [Qt 6.7.3 Windows dialog backend](https://github.com/qt/qtbase/blob/v6.7.3/src/plugins/platforms/windows/qwindowsdialoghelpers.cpp)
