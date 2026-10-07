# vibePlayerQT

[English](README.md) | [简体中文](README.zh-CN.md)

vibePlayerQT is a cross-platform desktop media center built with Qt Quick, C++23 and libmpv. It aims to provide a modern native alternative for browsing Emby, Jellyfin, WebDAV and IPTV sources while keeping playback fast, direct and maintainable.

The project is still under active development. The current codebase focuses on native desktop playback, Emby/Jellyfin media browsing, WebDAV file access and local IPTV playlist playback.

## Highlights

The console companion supports service management, WebDAV/SMB transfers, TSSL backups,
video encryption/restoration, media browsing, playback controls and background tasks.
On Windows, run `.\vibePlayerQT list` or
`.\vibePlayerQT shell` beside the installed EXE/COM files.
See the [complete CLI tutorial (中文)](VIBEDOCS/CLI-Tutorial.md) for commands,
parameters and automation examples, or the [CLI reference](VIBEDOCS/CLI.md) for implementation details.

- Native Qt 6 / Qt Quick desktop application.
- libmpv Window Embedding playback, managed through a single `PlayerController`.
- Emby and Jellyfin login, library browsing, continue watching, media details, series seasons and episodes.
- Playback resume and playback progress reporting for Emby and Jellyfin.
- WebDAV directory browsing, upload, download, folder transfer and direct playback through a local proxy when needed.
- IPTV M3U/M3U8 import, group filtering, search and playback.
- Built-in HTTP/HTTPS direct-media and HLS link playback with dated history, replay, deletion and traffic statistics.
- Saved service cards with multiple accounts per server, optional auto-login and drag sorting.
- Dark/light/system theme modes, Chinese/English UI text and minimize-to-tray support.
- CMake-based builds and GitHub Actions packaging for Windows, macOS and Linux.

## CLI Guide

Open a terminal in the installed `bin` directory:

```powershell
.\vibePlayerQT.com --version
.\vibePlayerQT.com list
.\vibePlayerQT.com help webdav
.\vibePlayerQT.com shell
```

The `.com` entry point runs the CLI; running without a command opens the desktop application.
The tutorial below is written in Chinese and includes PowerShell, CMD and GitHub Actions examples.

| Guide | Contents |
| --- | --- |
| [Complete CLI tutorial](VIBEDOCS/CLI-Tutorial.md) | All commands, parameters, configuration keys and troubleshooting |
| [Quick start](VIBEDOCS/CLI-Tutorial.md#quick-start) | Program entry point and your first WebDAV upload |
| [TSSL and video encryption/restoration](VIBEDOCS/CLI-Tutorial.md#video) | Package, preserve keys and restore videos |
| [Background tasks](VIBEDOCS/CLI-Tutorial.md#tasks) | Submit, pause, cancel and resume after restarting |
| [Automation and JSON](VIBEDOCS/CLI-Tutorial.md#automation) | Exit codes, scripts and GitHub Actions |
| [CLI implementation reference](VIBEDOCS/CLI.md) | Shared modules and verification |

## Project Status

| Area | Status |
| --- | --- |
| Emby | Login, libraries, item browsing, continue watching, details, playback URL creation and playback reporting are implemented. |
| Jellyfin | Login, libraries, item browsing, continue watching, details, playback URL creation and playback reporting are implemented. |
| WebDAV | PROPFIND browsing, MKCOL, GET/PUT transfers, transfer list, storage warning and video playback are implemented. |
| IPTV | Local M3U/M3U8 import, channel list, groups, search and playback are implemented. |
| Link playback | HTTP/HTTPS direct-media and HLS playback, dated history, individual deletion and traffic statistics are implemented. |
| SMB | Windows CLI supports browsing, transfers, copy/move/delete and playback through the native Windows SMB backend. Desktop SMB browsing is planned. |
| Packaging | CI builds Windows portable ZIP/NSIS/MSI, macOS ZIP/DMG, and Linux tar.gz/AppImage/DEB/RPM/Flatpak artifacts. |

## Architecture

The project follows a layered desktop application structure:

```text
QML UI
  -> ViewModel
    -> Service
      -> Repository
        -> Infrastructure
```

QML is responsible for presentation, animation, input and navigation. Network requests, database access, media parsing and playback orchestration live in C++.

Key directories:

```text
src/
  app/          application startup, tray and window behavior
  database/     SQLite/QSettings persistence
  models/       shared domain models
  network/      QNetworkAccessManager wrapper
  player/       libmpv integration and QML video item
  services/     Emby, Jellyfin, WebDAV, IPTV and credentials
  settings/     application settings
  utils/        logging and JSON helpers
  viewmodels/   QML-facing application state and list models
qml/            Qt Quick UI
docs/           build notes
scripts/        local build helpers
VIBEDOCS/       module design notes
```

## Tech Stack

- C++23
- Qt 6.5+ with Qt Quick, QML, Quick Controls 2, Network, SQL, Widgets and XML
- SQLite through Qt SQL
- libmpv
- CMake 3.24+

## Dependencies

### Windows

Install:

- Visual Studio 2022 C++ toolchain
- LLVM/clang-cl if using the provided Clang scripts
- Qt 6.7.3 or another Qt 6.5+ MSVC-compatible package
- CMake and Ninja

The local Windows build expects the libmpv development package under:

```text
third_party/mpv/dev/
  include/mpv/client.h
  libmpv.dll.a
  libmpv-2.dll
```

Large libmpv archives and runtime binaries are not committed. See `VIBEDOCS/PlayerRuntime.md` for the pinned package used by the CI and local setup.

### macOS and Linux

Install Qt 6, CMake, Ninja, pkg-config and libmpv development files through your system package manager. The GitHub Actions workflow uses Homebrew on macOS and apt on Ubuntu.

## Build

### Windows Clang

From a PowerShell terminal:

```powershell
.\scripts\configure-clang.cmd
.\scripts\build-clang.cmd
```

The executable is generated at:

```text
build-clang/vibePlayerQT.exe
```

More Windows build options are documented in `docs/BuildWindows.md`.

### Generic CMake

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package
```

On Linux and macOS, CMake locates libmpv through `pkg-config`.

## GitHub Releases

The workflow at `.github/workflows/build-release.yml` builds every push to `main`. Pushing a tag such as `v1.0.0` also publishes a GitHub Release with packaged artifacts for:

- Windows x86_64
- macOS x86_64
- macOS arm64
- Linux x86_64 tar.gz and AppImage
- Linux arm64 tar.gz and AppImage

## Documentation

Design notes and implementation records live in `VIBEDOCS/`.

Useful starting points:

- [Complete CLI tutorial (中文)](VIBEDOCS/CLI-Tutorial.md)
- [CLI command and implementation reference](VIBEDOCS/CLI.md)
- `docs/BuildWindows.md`
- `docs/DevelopmentEnvironment.md` (verified local toolchain inventory)
- `VIBEDOCS/MediaServices.md`
- `VIBEDOCS/PlayerRuntime.md`
- `VIBEDOCS/WebDAV.md`
- `VIBEDOCS/IPTV.md`
- `VIBEDOCS/GitHubActionsRelease.md`
- `TODO.md`

Some older local notes may contain encoding issues. The source code and this README should be treated as the current public-facing reference.

## Security Notes

- Do not commit real server credentials, access tokens, cookies, local SQLite databases or token-bearing logs.
- Emby/Jellyfin access tokens are currently stored in the local SQLite session database.
- WebDAV passwords use Windows Credential Manager when available; other platforms currently request the password again.
- Logs and UI must not expose passwords, tokens, cookies or full token-bearing playback URLs.

## Roadmap

- Improve token storage on macOS and Linux through Keychain and Secret Service.
- Add image proxy/cache support so token-bearing image URLs are not exposed to QML.
- Add richer playback error mapping and external subtitle support.
- Validate WebDAV/IPTV playback across more server implementations.
- Add desktop SMB browsing and portable SMB2/SMB3 backends through proven libraries.
- Expand automated tests and release smoke checks.
