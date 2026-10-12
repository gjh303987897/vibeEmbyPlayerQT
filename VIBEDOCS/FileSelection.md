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
  or open another picker safely. Completion checks the dialog identity and
  disconnects its handler before invoking the continuation. A late native
  reply from a canceled dialog cannot consume a newly opened picker's request.
- On Windows, the finished dialog stays alive for a one-second event-loop
  grace period before `deleteLater()`. Qt 6.7.3's native Shell thread can still
  be unwinding COM when `finished` arrives; immediate helper destruction waits
  up to 500 ms on the GUI thread and can log `Thread failed to finish`.
  Other platforms use `deleteLater()` immediately. The controller already
  considers the operation complete during the grace period.
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
late completion from a previously canceled picker,
owner hiding/destruction and controller teardown without business callbacks.
It operates only on temporary paths and does not upload, import or export
user data.

CTest uses the offscreen platform and explicitly locates it through the
installed `Qt6::QOffscreenIntegrationPlugin` target. The application deploys
the normal desktop platform plugin; the offscreen plugin is needed only for
this test. These tests verify the shared controller and Qt fallback behavior,
not the visual appearance of each operating system's native picker.

`QmlUtilityTest::nativePickerSurvivesVideoPlayback` additionally opens and
cancels the Windows Shell picker before and after real embedded video playback,
using a temporary FFmpeg-generated fixture and the installed libmpv runtime.
CTest registers it as `native_video_file_picker`; it waits for deferred cleanup
and treats `Thread failed to finish` as a failure. The full
`native_iptv_import_playback` test also exercises the add-service popup after
IPTV playback, including normal destruction of the root Quick window.

Both native tests use `tests/NativeFileDialogTestHelpers.h` to enumerate only
visible Windows Shell dialog windows (`#32770`) with the current test process
ID and expected caption. A visible QFileDialog host alone does not satisfy the
check. The helper is compiled only on Windows and never reads user files.

The add-service popup uses `DialogBackdropBlur` in `Main.qml`. Its two
property-owned `ShaderEffectSource` objects have an explicit visual parent,
are invisible themselves, and release `sourceItem` when the backdrop is hidden.
Qt 6.7.3's `setSourceItem` takes a window reference even when the capture object
has no window, while its destructor balances that reference only when it has a
window. The previous parentless captures retained scene references and the full
Windows test crashed in focus cleanup during root-window destruction. With
visual ownership and hidden-source release, that same workflow exits normally.

## Official references

- [QFileDialog modes, options and native defaults](https://doc.qt.io/qt-6/qfiledialog.html)
- [QDialog asynchronous open and lifetime](https://doc.qt.io/qt-6/qdialog.html#open)
- [QWindow transient parent](https://doc.qt.io/qt-6/qwindow.html#transientParent-prop)
- [Qt 6.7.3 Windows dialog backend](https://github.com/qt/qtbase/blob/v6.7.3/src/plugins/platforms/windows/qwindowsdialoghelpers.cpp)
