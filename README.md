# vibePlayerQT

vibePlayerQT is a cross-platform desktop media center built with Qt Quick, C++23 and libmpv.

The project currently focuses on Emby and Jellyfin login, service cards, media library browsing, continue-watching cards, media details and embedded video playback.

## Goals

- Modern desktop media center UI for Windows, macOS and Linux.
- Emby and Jellyfin support first, with WebDAV, SMB and IPTV planned.
- High-quality playback through libmpv Window Embedding.
- Maintainable layered architecture: QML UI, ViewModel, Service, Repository and infrastructure.

## Tech Stack

- C++23
- Qt 6 Quick / QML
- QNetworkAccessManager
- SQLite through Qt SQL
- libmpv
- CMake

## Current Features

- Saved service cards for Emby and Jellyfin.
- Multiple accounts for the same server.
- Optional auto-login per service card.
- Drag sorting for service cards.
- Chinese / English i18n.
- Dark / light theme settings.
- Continue-watching carousel.
- Media library list and item list.
- Media details page.
- libmpv embedded playback with pause, seek, fullscreen, volume, speed, subtitle and audio track controls.

## Repository Layout

```text
src/
  app/
  database/
  models/
  network/
  player/
  services/
  settings/
  utils/
  viewmodels/
qml/
resources/
scripts/
tests/
VIBEDOCS/
```

## Dependencies

Install Qt 6.7 or newer with MSVC-compatible libraries on Windows.

The current Windows clang build expects libmpv development files under:

```text
third_party/mpv/dev/
  include/mpv/client.h
  libmpv.dll.a
  libmpv-2.dll
```

Large libmpv binaries and downloaded archives are intentionally ignored by Git. See `VIBEDOCS/PlayerRuntime.md` for the runtime package details used during local setup.

## Build With Clang On Windows

From a PowerShell terminal:

```powershell
.\scripts\configure-clang.cmd
.\scripts\build-clang.cmd
```

The executable is generated at:

```text
build-clang/vibePlayerQT.exe
```

## Documentation

Project design notes live in `VIBEDOCS/`.

Start with:

- `VIBEDOCS/README.md`
- `VIBEDOCS/MediaServices.md`
- `VIBEDOCS/PlayerRuntime.md`
- `TODO.md`

## Security Notes

Do not commit real server credentials, access tokens, cookies, local SQLite databases or token-bearing logs.

The first implementation stores tokens in SQLite locally. A future version should migrate token storage to Keychain, Credential Manager or Secret Service.
