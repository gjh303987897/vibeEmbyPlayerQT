# Encrypted HLS (`.m3u8s`) and TSSL

## Scope

The encrypted HLS module has two entry points:

- The local M3U8S manager packages a normal video as an HLS VOD package.
- WebDAV playback opens a selected `.m3u8s` file through a loopback-only HTTP
  session for libmpv and decrypts registered MPEG-TS segments before returning
  them.

The `.m3u8s` file is still a valid HLS media playlist. Playlist and resource
URIs are not rewritten. The existing single-file WebDAV proxy is not modified.

The format protects media stored on a remote or cloud service. TSSL files are
local secret material. They are never uploaded automatically, and export is an
explicit user operation.

## TSSL v2

TSSL is UTF-8 JSON with this shape:

```json
{
  "format": "TSSL",
  "version": 2,
  "algorithm": "AES-256-GCM",
  "identifier": "exactly 4096 Base64URL characters",
  "rootManifestSha256": "64 lowercase hexadecimal characters",
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

The packager creates the identifier from 3072 cryptographically secure random
bytes and encodes it as unpadded Base64URL. This produces exactly 4096
characters in the `[A-Za-z0-9_-]` alphabet. The root `.m3u8s` carries the same
value in exactly one line immediately after `#EXTM3U`:

```
#M3U8S-IDENTIFIER:<4096-character identifier>
```

Playback requires both the exact root-manifest digest and the identifier to
match the local TSSL package. A missing, malformed, duplicate, or mismatched
identifier stops playback before segment data is exposed.

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
selects the source, output folder, and segment duration, then renders progress.
FFmpeg is started with separate program and argument values through `QProcess`.
The application checks its executable directory first and then `PATH` for
`ffmpeg` (`ffmpeg.exe` on Windows). A missing executable is reported before a
job starts.

The workflow is:

1. FFmpeg transcodes the first video stream to H.264 and the optional first
   audio stream to AAC, then writes a closed-GOP HLS VOD into a temporary
   staging directory.
2. Each TS file receives an independent random 256-bit key and 128-bit IV.
3. The TS is encrypted with AES-256-GCM and immediately decrypted in memory to
   verify its authentication tag and plaintext before the original is
   replaced.
4. The identifier is inserted into the root playlist, which is renamed to
   `.m3u8s`; matching TSSL metadata is generated and validated.
5. TSSL is saved atomically in local application storage and exported beside
   the media package. Only then is the staging directory renamed to its final
   output name.

Cancellation or failure removes the staging directory. It does not publish a
partially encrypted package. A completed output directory contains the
`.m3u8s`, encrypted `.ts` files, and an exported `.tssl` recovery file.

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

## Local storage, restore, and export

The M3U8S manager lists local TSSL packages and supports import/restore,
export, deletion, and opening the storage directory. The list exposes only a
short identifier preview; full keys and identifiers are not passed to QML.

Restore validates the entire TSSL document before atomically writing it to the
application-local `tssl` directory. The filename is derived from the root
manifest digest, not from untrusted input. Stored and exported files are set to
owner read/write permissions where supported. Invalid legacy or damaged files
remain visible for diagnosis and deletion but cannot be exported as valid v2
packages.

TSSL is not encrypted at rest because the current threat model is remote/cloud
storage, not a compromised local OS account. Exported files contain every
media key and must be handled as secrets.

## Limits and failure behavior

- TSSL documents are limited to 64 MiB.
- Playlists are limited to 4 MiB.
- Encrypted TS objects are limited to 512 MiB.
- Auxiliary resources are limited to 128 MiB.
- Packaging segment duration is limited to 2 through 30 seconds.
- Missing TSSL packages, digest or identifier mismatches, unregistered paths,
  unavailable OpenSSL EVP support, network failures, and authentication
  failures all stop playback. There is no unauthenticated fallback.

## Verification

`EncryptedHlsFormatTest` covers TSSL v2 parsing, storage, strict identifiers,
AES-GCM vectors, randomized encryption, and malformed packages.
`EncryptedHlsPlaybackProxyTest` covers identifier matching and authenticated
playback. `EncryptedHlsPackagerTest` covers in-place HLS encryption,
cancellation, tag-tamper rejection, and an end-to-end FFmpeg package.

## Standards references

- HLS second edition draft: https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis
- FFmpeg HLS muxer: https://ffmpeg.org/ffmpeg-formats.html#hls-2
- Qt `QProcess`: https://doc.qt.io/qt-6/qprocess.html
- NIST SP 800-38D: https://csrc.nist.gov/pubs/sp/800/38/d/final
- OpenSSL EVP authenticated encryption: https://docs.openssl.org/3.0/man3/EVP_EncryptInit/
