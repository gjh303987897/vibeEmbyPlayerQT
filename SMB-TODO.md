# SMB Support TODO

This document records the agreed first-stage SMB support design for future implementation.
No SMB code has been implemented yet.

## Agreed Scope

- Add SMB as a new media source alongside WebDAV, Emby, Jellyfin, and IPTV.
- First implementation should be similar to the current WebDAV support:
  - service card creation
  - login/password prompt
  - remote file browsing
  - file upload
  - file download
  - video playback through the existing player page
- Use libsmb2 for SMB2/SMB3. Do not implement the SMB protocol manually.
- Require users to enter a share-level URL, for example:
  - `smb://nas/Movies/`
  - `smb://192.168.1.10/Media/Movies/`
- Use one SMB URL input field instead of separate host/share/path fields.
- Support anonymous/guest access.
- Provide advanced options for SMB protocol/security behavior.
- Save credentials through a generalized credential-store key by service type, not the current WebDAV-only namespace.
- Create a separate `SmbPage` first instead of immediately generalizing the current `WebDavPage`.
- Remote upload name conflicts should use automatic renaming.
- Do not support resumable upload/download in the first version.
- For playback proxy requests, open a fresh SMB handle per HTTP request.
- There is currently no SMB test environment, so implementation should wait until a test target is available.

## Proposed Architecture

### Service Layer

Add a new SMB service module:

- `src/services/smb/SmbClient.h`
- `src/services/smb/SmbClient.cpp`
- `src/services/smb/SmbTransferManager.h`
- `src/services/smb/SmbTransferManager.cpp`
- `src/services/smb/SmbPlaybackProxy.h`
- `src/services/smb/SmbPlaybackProxy.cpp`

The first version may copy the WebDAV shape instead of extracting a shared remote-file abstraction.
Keep the SMB implementation isolated so a later refactor can merge common WebDAV/SMB concepts.

### Models

Add SMB-specific file item model first:

- `SmbItem`
- `SmbItemListModel`

The fields can mirror `WebDavItem`:

- `name`
- `url`
- `relativePath`
- `contentType` or display type
- `lastModified`
- `size`
- `directory`
- `playable`

Future improvement: replace `WebDavItem` and `SmbItem` with a shared `RemoteFileItem`.

### ViewModel

Add SMB state to `AppViewModel` in the same pattern as WebDAV:

- current SMB service card
- current SMB password
- current SMB URL/path
- SMB navigation history
- SMB item list
- SMB playback stream id

Expected invokable methods:

- `openSmbItem(int row)`
- `smbBack()`
- `refreshSmbDirectory()`
- `chooseSmbUploadFiles()`
- `chooseSmbUploadFolder()`
- `downloadSmbItem(int row)`

Keep QML free of protocol logic.

### UI

Add `SMB` to the service-type selector.

Use a single SMB URL field. Suggested placeholder:

- `smb://nas/Movies/`

Add optional advanced settings for SMB:

- protocol version: Auto, SMB2, SMB3, SMB3.1.1
- signing: Auto/Off/Required
- encryption/seal: Off/Required
- domain/workgroup
- port, if not supplied in the URL
- anonymous/guest mode

First version can keep advanced settings hidden behind an expandable section.

Create a separate `SmbPage` first. It can copy the current WebDAV page layout:

- current path
- refresh
- upload file
- upload folder
- transfers
- file rows
- download action

Future improvement: merge `WebDavPage` and `SmbPage` into a shared remote file browser page.

### Credentials

Generalize `CredentialStore` target naming.

Current behavior is WebDAV-specific:

- `vibePlayerQT/WebDAV/<key>`

Future behavior should include the service type:

- `vibePlayerQT/<ServiceType>/<key>`

This will allow:

- `vibePlayerQT/WebDAV/<server-id>`
- `vibePlayerQT/SMB/<server-id>`

Do not log passwords. Do not store SMB passwords in SQLite.

Non-Windows behavior can initially match WebDAV:

- credential store unavailable
- password requested when opening the service

Future improvement: add macOS Keychain and Linux Secret Service support.

## SMB URL Handling

The app should require a share-level URL. The URL should include the SMB share:

- valid: `smb://nas/Movies/`
- valid: `smb://nas:445/Movies/Folder/`
- invalid for first version: `smb://nas/`

The URL parser should separate:

- host
- optional port
- share name
- path inside share

Domain/workgroup should come from the advanced option, not from a required separate field. If libsmb2 URL parsing supports domain syntax, keep app-side behavior consistent and document it before exposing it.

Open question for implementation: decide whether `smb://domain;user@host/share/path` should be accepted, rejected, or normalized into the username/domain fields.

## Login Behavior

Support three login modes:

- username/password
- guest/anonymous
- password prompt for saved card without stored credential

For auto-login:

- if credentials are stored, use them
- if guest/anonymous is enabled, open without password
- otherwise emit the existing password-required flow

The first directory listing should act as connection validation.

## Browsing

Directory listing should use libsmb2 directory APIs.

Sorting should match WebDAV:

- directories first
- locale-aware name sorting

Video detection should initially reuse the existing extension list:

- `mp4`
- `mkv`
- `avi`
- `mov`
- `webm`
- `ts`
- `m2ts`
- `flv`
- `wmv`
- `mpg`
- `mpeg`
- `m4v`
- `3gp`
- `ogv`

Future improvement: move video extension detection into a shared utility.

## Uploads

Support:

- file upload
- folder upload
- recursive folder creation

On remote name conflict, automatically rename instead of overwriting.

Suggested remote rename pattern:

- `file.ext`
- `file (1).ext`
- `file (2).ext`

For directories:

- `Folder`
- `Folder (1)`
- `Folder (2)`

Implementation should stat the remote target before upload/create.

Do not support resumable upload in the first version.

## Downloads

Support:

- file download
- folder download
- recursive folder traversal

Local name conflicts should reuse the current WebDAV-style automatic local rename behavior.

Before folder download, estimate size recursively when possible.
If total size is unknown or cannot be fully confirmed, show the existing storage warning pattern.

Do not support resumable download in the first version.

## Playback

Do not rely on mpv directly opening `smb://` URLs.

Create an SMB local HTTP playback proxy similar to `WebDavPlaybackProxy`.
The proxy should:

- listen on `127.0.0.1`
- generate short-lived local stream URLs
- support `GET`
- support `HEAD` if practical
- support HTTP `Range`
- map range reads to libsmb2 file reads
- avoid logging SMB credentials or full sensitive URLs

For each HTTP request from mpv:

- create/connect SMB context
- open the requested SMB file
- seek/read the requested byte range
- close the SMB handle when the request finishes

This is simpler and more isolated than sharing SMB handles across concurrent mpv requests.
It may be slower, but that is acceptable for the first implementation.

Future improvement:

- connection pool per server/share
- shared read handles for sequential playback
- better cache behavior for high-bitrate 4K files

## Transfer Manager

Do not force SMB transfers through `QNetworkAccessManager`.

Options:

- create `SmbTransferManager` with libsmb2 and worker-thread execution
- later extract a shared transfer task model/queue used by WebDAV and SMB

The first version can keep the existing `TransferTaskListModel`.
SMB tasks should still report:

- queued/running/done/failed/canceled
- bytes done
- bytes total
- progress
- source
- target

Because libsmb2 operations are not Qt network replies, cancellation and progress need explicit implementation.

## Threading

Do not perform SMB I/O on the UI thread.

Preferred first approach:

- run SMB operations on worker threads
- report completion/progress back to `AppViewModel` through queued Qt signals

Areas that must stay async:

- directory listing
- stat
- upload
- download
- recursive size estimation
- playback proxy reads

## Error Handling

Use `std::expected`-style results where practical, consistent with existing service code.

Map SMB failures into clear user-facing categories:

- invalid URL
- authentication failed
- permission denied
- host unreachable
- share not found
- path not found
- operation timed out
- read/write failed
- unsupported protocol/security option

Avoid raw numeric errors in the UI unless there is no better message.

## Build And Dependency Notes

Future implementation must decide how libsmb2 is provided on each platform.

Possible approaches:

- system package on Linux
- Homebrew or bundled library on macOS
- bundled/prebuilt library on Windows
- vcpkg/conan only if accepted by project dependency policy

The project currently bundles libmpv for Windows under `third_party`.
If libsmb2 is bundled, document:

- upstream source
- version/commit
- license
- build options
- SHA-256 checksums for downloaded artifacts
- runtime DLL/shared-library deployment rules

Update CMake only after the dependency decision is made.

## Testing Needed

No SMB test environment exists yet.
Before implementation or merge, prepare at least one test target.

Minimum manual test matrix:

- connect to `smb://nas/share/`
- connect to nested path `smb://nas/share/folder/`
- username/password login
- guest/anonymous login
- wrong password handling
- invalid share handling
- directory browsing
- browsing paths with spaces and non-ASCII characters
- file upload
- folder upload
- automatic remote rename on conflict
- file download
- folder download
- automatic local rename on conflict
- cancel upload/download
- play video
- seek during video playback
- play large file
- verify no password appears in logs

Recommended local test setup later:

- Samba container or VM
- one read-only share
- one read/write share
- one guest share
- one user/password share
- sample videos large enough to test seeking

## Documentation Updates Needed Later

When implementation begins, update:

- `VIBEDOCS/SMB.md`
- `VIBEDOCS/MediaServices.md`
- `README.md`
- `README.zh-CN.md`
- build docs if libsmb2 setup is non-trivial

`VIBEDOCS/SMB.md` should explain:

- module boundaries
- credential strategy
- URL requirements
- transfer behavior
- playback proxy behavior
- limitations of first version

## Future Refactor Ideas

After the first SMB implementation works:

- extract `RemoteFileItem`
- extract `RemoteFileItemListModel`
- extract common remote file page actions
- merge WebDAV and SMB transfer display behavior
- introduce a protocol-neutral `RemoteFileService` interface
- move video extension detection to a shared utility
- generalize default download settings from `webdav/defaultDownloadDirectory` to a remote-file setting
- improve credential storage on macOS/Linux
- add resumable transfers
- add connection pooling for SMB browsing/playback
- add SMB share discovery if a reliable cross-platform approach is chosen
