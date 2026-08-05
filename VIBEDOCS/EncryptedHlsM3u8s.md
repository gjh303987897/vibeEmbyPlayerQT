# Encrypted HLS (`.m3u8s`) and TSSL

## Scope

The encrypted HLS module has two entry points:

- The local M3U8S manager packages a normal video as an HLS VOD package.
- Local and WebDAV playback open a selected `.m3u8s` file through a loopback-only HTTP
  session for libmpv and decrypts registered MPEG-TS segments before returning
  them.

The `.m3u8s` file is still a valid HLS media playlist. Playlist and resource
URIs are not rewritten. The existing single-file WebDAV proxy is not modified.

The format protects media stored on a remote or cloud service. TSSL files are
local secret material. They are never uploaded automatically, and export is an
explicit user operation.

## TSSL v3

TSSL is UTF-8 JSON with this shape:

```json
{
  "format": "TSSL",
  "version": 3,
  "algorithm": "AES-256-GCM",
  "identifier": "exactly 4096 Base64URL characters",
  "rootManifestSha256": "64 lowercase hexadecimal characters",
  "sourceName": {
    "encrypted": "base64 encoded IV, ciphertext, and authentication tag",
    "key": "base64 encoded 32-byte key"
  },
  "manifests": [
    { "path": "variants/720p.m3u8", "sha256": "..." }
  ],
  "segments": [
    { "path": "segment_000001.ts", "key": "base64 encoded 32-byte key" }
  ],
  "resources": [
    { "path": "subtitles/zh.vtt", "sha256": "..." }
  ]
}
```

TSSL v2 remains readable for packages created before source-filename
encryption was introduced. A v2 package has no `sourceName` object and falls
back to the selected manifest name for display. New packages are always v3.
Mixing v2 and v3 fields is rejected instead of silently downgrading security.

The packager creates the identifier from 3072 cryptographically secure random
bytes and encodes it as unpadded Base64URL. This produces exactly 4096
characters in the `[A-Za-z0-9_-]` alphabet. The root `.m3u8s` carries the same
value in exactly one metadata line near the start of the playlist:

```
#M3U8S-IDENTIFIER:<4096-character identifier>
#M3U8S-SOURCE-NAME:<unpadded Base64URL authenticated ciphertext>
```

Playback requires both the exact root-manifest digest and the identifier to
match the local TSSL package. A missing, malformed, duplicate, or mismatched
identifier stops playback before segment data is exposed.

The exact original basename, including its extension and UTF-8 characters, is
encrypted with a separate random 256-bit key and 128-bit IV. The identifier is
included as AES-GCM additional authenticated data, binding the name to the
package. The manifest stores only `16-byte IV | ciphertext | 16-byte tag`; its
key is present only in local TSSL. TSSL stores the same ciphertext so
restore/export is self-contained. Playback requires the manifest and TSSL
ciphertexts to match, verifies the GCM tag, validates strict UTF-8 and a safe
basename, and only then exposes the recovered name for display and history.
The recovered name is never used to resolve a filesystem path.

`rootManifestSha256` hashes the exact HTTP entity bytes of the selected
`.m3u8s` file. `manifests` contains every child playlist and its exact digest.
`resources` is optional and permits integrity-checked auxiliary files such as
WebVTT subtitles. Every requested `.ts` path must occur exactly once in
`segments`.

All registered paths are decoded, forward-slash-separated paths relative to
the root manifest directory. Absolute paths, backslashes, query strings,
fragments, non-canonical dot segments, duplicates, and paths escaping the root
are rejected. Playlist URIs may contain queries, but TSSL lookup uses their
normalized path.

## Local packaging

`EncryptedHlsPackager` owns the asynchronous packaging workflow. QML only
selects the source, output folder, encoding options, quality, and segment
duration, then renders progress. The selected output folder and encoding
options are persisted with `SessionRepository` and restored on the next launch.
The manager can open the configured output root directly; after a package is
created, a separate action opens that package's final digest-named directory.
FFmpeg is started with separate program and argument values through `QProcess`.
The application checks its executable directory first and then `PATH` for
`ffmpeg` (`ffmpeg.exe` on Windows). A missing executable is reported before a
job starts.

The page offers these encoding modes:

- Video stream copy preserves the original encoded video without decode or
  re-encode. Segment boundaries follow existing keyframes, so the requested
  duration is approximate. FFmpeg reports an error instead of silently
  transcoding when the source stream is incompatible with MPEG-TS HLS.
- H.264 uses `libx264`; high, balanced, and compact quality map to CRF 18, 20,
  and 24.
- H.265 uses `libx265`; high, balanced, and compact quality map to CRF 20, 23,
  and 28.
- Audio stream copy preserves the original encoded audio with the same
  compatibility limitation. AAC transcodes the optional first audio stream at
  192 kbit/s.

The workflow is:

1. FFmpeg processes the first video stream and optional first audio stream with
   the selected modes, then writes an MPEG-TS HLS VOD into a temporary staging
   directory. Transcoded video uses a closed GOP with forced segment-boundary
   keyframes, disables scene-cut keyframes with the encoder-specific option,
   and declares independent segments. Stream-copy video does neither, because
   the source keyframe layout cannot be changed without transcoding.
2. Each TS file receives an independent random 256-bit key and 128-bit IV.
3. The TS is encrypted with AES-256-GCM and immediately decrypted in memory to
   verify its authentication tag and plaintext before the original is
   replaced.
4. The source basename is encrypted and immediately authenticated, then the
   identifier and filename ciphertext are inserted into the root playlist.
   The upload-facing playlist is always named `index.m3u8s`.
5. TSSL is saved atomically only in local application storage. Only then is
   the media staging directory renamed using the root-manifest digest rather
   than the source name.

Cancellation or failure removes the staging directory. It does not publish a
partially encrypted package. A completed upload-ready directory contains only
the `.m3u8s` and encrypted `.ts` files. A TSSL recovery copy is created only
through the manager's explicit export action, so uploading the media directory
cannot accidentally disclose its keys.

Neither the completed directory name nor the root playlist name contains the
source basename. Ciphertext is deliberately kept inside the manifest rather
than used as a filename, avoiding cross-platform filename length and character
constraints.

## Segment encryption

Each segment is encrypted independently with AES-256-GCM and has this byte
layout:

```
16-byte IV | ciphertext | 16-byte authentication tag
```

The TSSL entry supplies the 32-byte key and no additional authenticated data
is used. A key and IV pair must never be reused.

During playback, the proxy downloads the complete encrypted object and calls
the OpenSSL EVP GCM finalization check before writing any plaintext to libmpv.
Failed or truncated tags produce an HTTP error and the tentative plaintext
buffer is cleared. A byte-range response is sliced only after the complete
object has passed authentication.

## Playlist and URI contract

The proxy returns verified playlist bytes without textual URI rewriting. This
is possible because libmpv receives a local URL whose directory mirrors the
package root. The following restrictions make resolution deterministic:

- The first non-empty line is `#EXTM3U`.
- Media, child-playlist, audio, subtitle, map, and other URI values are
  relative to the root package.
- URI resolution may use `..` only when the resolved path remains under the
  package root.
- External schemes, authorities, and root-absolute paths are rejected.
- `#EXT-X-KEY` and `#EXT-X-SESSION-KEY` are rejected because keys belong only
  in TSSL.
- Child playlists and auxiliary resources are served only when their digest is
  registered. Unknown paths are denied.

## Local playback

`LocalMediaService` recognizes `.m3u8s` for configured folders, drag-and-drop,
and history replay. Generated `segment_NNNNNN.ts` files are hidden when the
directory contains an M3U8S manifest so encrypted segments are not offered as
standalone videos.

`EncryptedHlsPlaybackProxy` uses one verification and HTTP-serving path for
local and WebDAV packages. Local files are read on worker threads. Every
requested resource must be registered by TSSL, resolve to a canonical readable
file, and remain inside the canonical package directory after symbolic-link
resolution. libmpv still receives an ordinary localhost HLS URL; no player-core
changes are required. Playback history stores the real local `.m3u8s` path,
never the temporary localhost session URL.

## Local storage, restore, and export

The M3U8S manager lists local TSSL packages and supports import/restore,
export, deletion, and opening the storage directory. The list exposes only a
recovered source basename and a short identifier preview; full keys and
identifiers are not passed to QML.

Restore validates the entire TSSL document before atomically writing it to the
application-local `tssl` directory. The filename is derived from the root
manifest digest, not from untrusted input. Stored and exported files are set to
owner read/write permissions where supported. Invalid legacy or damaged files
remain visible for diagnosis and deletion but cannot be exported as valid
packages. Import and export preserve v2/v3 and the complete authenticated
source-name metadata.

TSSL is not encrypted at rest because the current threat model is remote/cloud
storage, not a compromised local OS account. Exported files contain every
media key and must be handled as secrets.

## Limits and failure behavior

- TSSL documents are limited to 64 MiB.
- Playlists are limited to 4 MiB.
- Encrypted TS objects are limited to 512 MiB.
- Auxiliary resources are limited to 128 MiB.
- Decrypted source basenames are limited to 4096 UTF-8 bytes.
- Packaging segment duration is limited to 2 through 30 seconds.
- Stream-copy packaging can fail when the original video or audio codec cannot
  be represented in MPEG-TS HLS. There is no automatic transcode fallback.
- Missing TSSL packages, digest or identifier mismatches, unregistered paths,
  unavailable OpenSSL EVP support, network failures, and authentication
  failures all stop playback. There is no unauthenticated fallback.

## Verification

`EncryptedHlsFormatTest` covers TSSL v2 compatibility, TSSL v3 source-name
authentication, restore/export, strict identifiers, AES-GCM vectors,
randomized encryption, and malformed packages. `EncryptedHlsPlaybackProxyTest`
covers identifier matching plus authenticated WebDAV and local playback.
`EncryptedHlsPackagerTest` covers in-place HLS encryption, opaque output names,
source-name recovery, cancellation, tag-tamper rejection, FFmpeg argument
selection for copy/H.264/H.265 modes, and an end-to-end FFmpeg package.
`LocalMediaServiceTest` covers local M3U8S discovery and generated-segment
filtering.

## Standards references

- HLS second edition draft: https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis
- FFmpeg HLS muxer: https://ffmpeg.org/ffmpeg-formats.html#hls-2
- Qt `QProcess`: https://doc.qt.io/qt-6/qprocess.html
- NIST SP 800-38D: https://csrc.nist.gov/pubs/sp/800/38/d/final
- OpenSSL EVP authenticated encryption: https://docs.openssl.org/3.0/man3/EVP_EncryptInit/
