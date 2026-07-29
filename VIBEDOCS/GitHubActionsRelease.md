# GitHub Actions Release Pipeline

## Scope

This document records the automated push build and packaging pipeline.

The pipeline lives at `.github/workflows/build-release.yml` and builds:

- Windows x86_64: portable ZIP, NSIS `.exe`, and MSI
- macOS x86_64: ZIP and unsigned `.dmg`
- macOS arm64: ZIP and unsigned `.dmg`
- Linux x86_64: tar.gz, AppImage, `.deb`, `.rpm`, and `.flatpak`
- Linux arm64: tar.gz, AppImage, `.deb`, `.rpm`, and `.flatpak`

Each push to `main` builds all packages. Pushing a version tag such as `v1.0.0` builds the same packages and publishes them to the matching GitHub Release.

## Build Model

The workflow separates compilation from packaging. A five-platform `build` matrix uses CMake as the single build and install entry point, runs the tests, validates the installed tree, and uploads a short-lived `build-input-*` artifact. Package-format jobs then consume that installed tree independently:

- `portable`: Windows/macOS ZIP and Linux tar.gz
- `native`: NSIS EXE, MSI, DMG, DEB, and RPM, with one matrix job per format and architecture
- `appimage`: one job per Linux architecture
- `flatpak`: one job per Linux architecture

This separation keeps a Flatpak, installer, or archive failure in its own GitHub Actions job instead of mixing every format into the platform compilation job. Windows and macOS build inputs are transported inside ZIP files; Linux uses tar.gz so executable modes and symbolic links survive `upload-artifact` transfer.

Main commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
cmake --install build --config Release --prefix package
```

Packaging always starts from the `package` install directory instead of collecting build-tree files manually.

Native installers are configured by `cmake/Packaging.cmake`. Their independent jobs pass the restored install tree to CPack through `CPACK_INSTALLED_DIRECTORIES` and disable a second `CPACK_INSTALL_CMAKE_PROJECTS` install pass:

- `NSIS` and `WIX` on Windows
- `DragNDrop` on macOS
- `DEB` and `RPM` on Linux

The installer metadata defaults to the numeric version from `project()`. A version tag such as `v1.2.3` overrides it with `1.2.3`; commit builds keep the project version internally while the artifact filename contains the short commit SHA.

## Qt Deployment

CMake uses `qt_generate_deploy_qml_app_script()` after `install(TARGETS)`.

This lets Qt deploy:

- Qt runtime libraries
- Qt plugins
- QML runtime imports used by `Main.qml`
- platform `qt.conf`

Windows and macOS use Qt's platform deployment tools underneath. Linux uses Qt's CMake runtime dependency deployment.

## libmpv Dependency

Windows resolves the latest standard x86_64 development package from the official `zhongfly/mpv-winbuild` GitHub Releases API. The asset must match `mpv-dev-x86_64-YYYYMMDD-git-HASH.7z`; the CPU-specific `-v3` package is intentionally excluded so the application remains compatible with older x86_64 processors.

The upstream project periodically removes old daily release tags, so the workflow does not keep a direct URL to a dated asset. GitHub's release asset metadata supplies a SHA-256 digest. The Windows job verifies the downloaded archive against that digest, extracts it into `third_party/mpv/dev`, checks for `include/mpv/client.h`, `libmpv.dll.a`, and `libmpv-2.dll`, then links the import library and installs the runtime DLL.

Windows CI configures CMake with `clang-cl` and `lld-link`, matching the local `scripts/configure-clang.cmd` flow while still using the MSVC-compatible Qt package.

macOS and Linux use system libmpv from Homebrew or apt through `pkg-config`.

macOS relies on Qt's generated deployment script to copy `libmpv` and its non-system dependencies into `vibePlayerQT.app/Contents/Frameworks`. ZIP and DMG artifacts are currently unsigned. Apple Developer ID signing and notarization must be added before treating the DMG as a signed public distribution.

Linux sets install RPATH to `$ORIGIN/../lib` so the installed executable can load libraries deployed beside it.

## Native Linux Packages

The DEB and RPM packages install under `/usr` and include the same deployed Qt, QML, plugin, and libmpv runtime files as the portable archive. CPack asks `dpkg-shlibdeps` to derive DEB system dependencies from the installed binaries. The staged `usr/lib` directory is passed as a private-library search path so ARM64 Qt and libmpv dependencies are resolved from the package payload instead of an incompatible runner library set. RPM keeps its standard automatic requirements/provides scan.

The packages are built on Ubuntu 24.04. They are intended for compatible Debian/Ubuntu and Fedora/RHEL/openSUSE systems, but the oldest supported glibc is therefore bounded by the Ubuntu 24.04 build environment. Broader old-distribution compatibility would require building on an older baseline or in distribution-specific containers.

## Flatpak Bundle

`packaging/flatpak/build-bundle.sh` converts the already deployed Linux package directory into an installable single-file Flatpak bundle. The application ID is `io.github.gjh303987897.vibeEmbyPlayerQT`, the runtime is `org.freedesktop.Platform` 25.08, and the bundle points installers to the Flathub runtime repository. Before invoking `flatpak build-init`, each Flatpak job adds the user-scoped Flathub remote and installs the matching architecture of both `org.freedesktop.Platform//25.08` and `org.freedesktop.Sdk//25.08`. The script renames exported desktop/icon files to the application ID and scales the application icon to Flatpak's 512x512 maximum.

Installing both refs is required: `flatpak build-init` does not download the SDK automatically. Without this setup it fails with `org.freedesktop.Sdk/<arch>/25.08 not installed`.

The bundle grants network, IPC, X11, Wayland, PulseAudio, GPU, and host filesystem access. Host filesystem access is required for the current media-source and local-file behavior. The bundle contains the application but not the Flatpak runtime; installation may download the matching runtime.

## Release Strategy

The workflow publishes formal releases from Git tags matching `v*.*.*`.

For the 1.0.0 release, create and push:

```bash
git tag v1.0.0
git push origin main
git push origin v1.0.0
```

The release job waits for all portable, native, AppImage, and Flatpak package jobs. It downloads only artifacts whose names start with `release-`, so the short-lived internal `build-input-*` archives are never published in the GitHub Release.

## Release Privacy Guard

GitHub always shows generated "Source code" zip/tar links for a Release tag. The repository uses `.gitattributes` with `export-ignore` so those generated source archives do not include local test fixtures, CI internals, development notes, scripts or VIBEDOCS content.

The release job also inspects the generated source archive and every uploaded binary asset before publishing. It uses format-aware listing for ZIP/tar, NSIS/MSI/DMG, DEB, RPM, and Flatpak artifacts. If a package contains development or local-data paths such as `tests`, `resources`, `fixtures`, `samples`, `cache`, `VIBEDOCS`, `.github`, or scripts, the release job fails before uploading assets. When updating an existing Release, the job deletes old assets before uploading the newly verified files.

## Notes

Runner labels were checked against GitHub's hosted runner reference:

- `windows-2022`
- `macos-15-intel`
- `macos-15`
- `ubuntu-24.04`
- `ubuntu-24.04-arm`

Qt packages are installed with `aqtinstall` using Qt 6.7.3:

- Windows: `win64_msvc2019_64`, installed as `msvc2019_64`
- macOS: `clang_64`, installed as `macos`
- Linux x86_64: `linux_gcc_64`, installed as `gcc_64`
- Linux arm64: `linux_gcc_arm64`, installed as `gcc_arm64`

The workflow pins Python 3.13 and `aqtinstall` 3.3.0. Qt archives are extracted with the runner's external `7z` command instead of aqt's default `py7zr` backend. This avoids the intermittent Windows `Bad7zFile: Specified path is bad` failure tracked by [aqtinstall issue #995](https://github.com/miurahr/aqtinstall/issues/995). Linux and macOS install p7zip before the Qt step, while the Windows hosted runner already supplies the same `7z` command used later for the libmpv package.

The Windows 2022 hosted runner supplies NSIS 3 and WiX Toolset 3. Each Linux packaging job installs only its format-specific tools: `dpkg-dev` for DEB, `rpm` for RPM, and `flatpak`, `imagemagick`, plus `ostree` for Flatpak. The release inspection job installs the readers needed to validate every published format.
