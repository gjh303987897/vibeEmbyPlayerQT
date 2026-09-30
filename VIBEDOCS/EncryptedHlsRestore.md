# Encrypted HLS video restoration

## Scope and entry point

The built-in **Video Restore / 视频还原** service card opens an independent local
restoration page. It accepts a mixed batch of `.m3u8s` manifests and indexed
`.m3u8sp` packages. M3U8S manifests must retain their segment directory. A matching
local TSSL package is required; the page reuses the existing TSSL import action
and FFmpeg configuration/capability probe.

The default output mode restores the authenticated original basename and its
container extension. MP4, M4V, MKV, MOV, TS, M2TS/MTS, AVI, WebM, FLV, OGV and
MPEG/MPG have explicit muxer mappings. An unsupported original container fails
with a suggestion to select MKV or MP4. No extension-only conversion occurs.
Codec/container combinations still have to be supported by FFmpeg: for example,
H.264 from an encrypted package cannot be copied into WebM.

TSSL v2 predates original filename metadata. These packages use the package
basename with `.mkv` in original mode, and the result row identifies this fallback.
Users can explicitly select MKV or MP4 for any package.

**Restoration recovers the media retained in the package. It cannot reverse
lossy encoding, recreate deleted tracks/subtitles or produce the byte-identical
file that existed before packaging.** The current packager keeps the first video
and audio tracks and removes subtitles. The restoration process performs no
additional encoding.

## Module boundaries

| Component | Responsibility |
| --- | --- |
| `EncryptedHlsRestorer` | Sequential queue, asynchronous FFmpeg process, streaming decryption, validation failure handling, cancellation, output publication |
| `EncryptedHlsPlaybackProxy` | Existing authenticated local package reader, bounded background reads/decryption and loopback HTTP serving |
| `EncryptedHlsRestoreViewModel` | File-selection continuations, settings, input validation, UI actions and observable state |
| `EncryptedHlsRestoreItems` | Virtualized list model with per-file states, errors, output paths and cached summary counts |
| `AppViewModel` | Owns/exposes the ViewModel, page navigation, shared bilingual text, FFmpeg and TSSL operations |
| QML restore page | Presentation, responsive layout, progress, controls, queue rows and service-card appearance |

The restorer owns a separate proxy, so its sessions do not revoke playback
sessions. It neither calls libmpv nor changes PlayerController. The batch is
sequential to bound disk contention, decrypt buffers and process memory.
Filesystem reads and AES-GCM work are reused from the proxy's background tasks;
FFmpeg is driven by QProcess signals without blocking the UI thread.

## Workflow

1. Validate the selected format, existing writable destination and FFmpeg path.
2. Prepare the local package asynchronously: validate the root manifest, matching
   TSSL metadata, original filename and, for M3U8SP, archive index/length.
3. Derive a portable output basename. Replace cross-platform forbidden characters,
   strip trailing spaces/dots, and prefix Windows reserved device names.
4. Resolve collisions by adding a numbered suffix. Existing files are preserved.
5. Create a `QTemporaryDir` under the destination filesystem. FFmpeg reads the
   authenticated localhost HLS stream and writes one staged output file using
   `-c copy`, the first video and available audio/subtitle streams. Input protocols
   are limited to HTTP/TCP; only registered, validated proxy resources are served.
6. On a normal zero exit, require reported video frames and a non-empty file,
   then rename the staged file to its final name. If a destination appeared in
   the meantime, fail safely without overwriting it.
7. Revoke the proxy session and remove staging on every terminal path. Continue
   with the next queued item after an individual failure.

The `streamFailed(sessionId, reason)` proxy signal is essential for output
integrity. An HLS demuxer may skip an unavailable segment and still exit normally.
Any missing/unregistered resource, manifest digest mismatch or segment GCM
failure aborts the corresponding restoration process; no partial video is
published. Playback's existing HTTP rejection behavior remains the same.

FFmpeg progress comes from `-progress pipe:1` with the input duration from its
diagnostics. Batch progress includes finished items plus active item time. A
120-second inactivity watchdog terminates a process that stops producing output.
Diagnostics are bounded to 32 KiB and are not shown/logged verbatim because they
may contain the session URL. Logs record operation, item number and exit status,
without keys, decrypted segment bytes or session URLs.

## Cancellation and lifecycle

Cancel kills the active process, removes its temporary output and marks queued
items canceled. Earlier successful files remain saved. A generation counter
invalidates queued dispatches and late package-preparation callbacks; late
successful preparations are immediately revoked. Cancel between items preserves
the just-completed result. The destructor disconnects process callbacks before
stopping FFmpeg and cleaning staging. Application shutdown cancels the batch.

Navigation can leave the batch running. Returning to the card/page shows the
same state. Changing sources/format/destination is disabled during a batch.
The shared `FileDialogController` provides native asynchronous multiple-file and
single-folder selection; pending pickers close when the page changes.

Settings are stored by SessionRepository under `encryptedHlsRestore/format` and
`encryptedHlsRestore/outputDirectory`. File selections and queue history are
session-only. No keys are copied into the output folder; original packages and
the TSSL store are retained.

## Verification

`encrypted_hls_restorer_test` uses real FFmpeg fixtures with H.264 video and AAC
audio and the application's own packager for both encrypted formats. It checks:

- MP4 filename/container restoration and decoded video SHA-256 equality;
- audio decoding, original MOV output, explicit MKV/MP4 output, unsupported
  container failure/retry and non-overwriting numbered names;
- invalid batch options, missing inputs and continuing to later valid items;
- missing TSSL/root manifest tampering;
- missing/modified late directory segments and modified archive entries;
- cancellation, queue restart, preserved completed files and staging cleanup;
- legacy v2 names and Windows reserved output names.

Actual Qt Quick captures verify the card, empty/result views, dark/light themes
and the minimum 980×640 window layout. The verification instrumentation is
temporary and removed from the delivered executable.

The existing encrypted format, archive, playback proxy, packager and file-dialog
tests cover the shared modules. On Windows, QtTest results are also written to
`build-clang/encrypted-hls-restorer-results.txt` for diagnostics when a detached
test executable's standard output is unavailable.

## Official implementation references

- [FFmpeg streamcopy](https://ffmpeg.org/ffmpeg.html#Streamcopy): copying encoded
  packets into a new container without decoding/encoding.
- [FFmpeg progress and process options](https://ffmpeg.org/ffmpeg.html): `-progress`,
  `-nostdin`, mapping, output format and error handling.
- [Qt QProcess](https://doc.qt.io/qt-6/qprocess.html): asynchronous process channels,
  separate arguments, errors, finished notification and termination.
- [Qt QTemporaryDir](https://doc.qt.io/qt-6/qtemporarydir.html): unique staging
  directory and automatic cleanup.
- See `EncryptedHlsM3u8s.md`, `EncryptedHlsTarContainer.md`, `FileSelection.md` and
  `FfmpegCapability.md` for the reused format, picker and external-tool contracts.
