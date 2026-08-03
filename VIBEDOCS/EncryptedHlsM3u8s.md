# Encrypted HLS (`.m3u8s`) and TSSL

## Scope

The encrypted HLS service is a WebDAV playback extension. A user selects a
`.m3u8s` file whose bytes are still a valid HLS playlist. The application
opens a loopback-only HTTP session for libmpv, preserves all relative URI
semantics, and decrypts registered MPEG-TS segments before returning them.
The existing single-file WebDAV proxy is not modified.

This format protects media stored on the remote WebDAV server. TSSL files are
local secret material. They are not uploaded automatically and their export is
an explicit user operation.

## TSSL v1

TSSL is UTF-8 JSON with this shape:

```json
{
  "format": "TSSL",
  "version": 1,
  "algorithm": "AES-256-GCM",
  "rootManifestSha256": "64 lowercase hexadecimal characters",
  "manifests": [
    { "path": "variants/720p.m3u8", "sha256": "..." }
  ],
  "segments": [
    { "path": "segments/000001.ts", "key": "base64 encoded 32-byte key" }
  ],
  "resources": [
    { "path": "subtitles/zh.vtt", "sha256": "..." }
  ]
}
```

`rootManifestSha256` is mandatory and hashes the exact HTTP entity bytes of
the selected `.m3u8s` file. `manifests` contains every child playlist and its
exact digest. `resources` is optional and permits integrity-checked auxiliary
files such as WebVTT subtitles. Every requested `.ts` path must occur exactly
once in `segments`.

All registered paths are decoded, forward-slash-separated paths relative to
the root manifest directory. Absolute paths, backslashes, query strings,
fragments, non-canonical dot segments, duplicates, and paths escaping the root
are rejected. Playlist URIs may contain queries, but TSSL lookup uses their
normalized path.

## Segment encryption

Each segment is encrypted independently with AES-256-GCM and has this byte
layout:

```
16-byte IV | ciphertext | 16-byte authentication tag
```

The TSSL entry supplies the 32-byte key and no additional authenticated data
is used. A key and IV pair must never be reused. The packager is responsible
for generating a fresh unpredictable IV for every segment.

The player downloads the complete encrypted object and calls the OpenSSL EVP
GCM finalization check before writing any plaintext to libmpv. Failed or
truncated tags produce an HTTP error and the tentative plaintext buffer is
cleared. A byte-range response is sliced only after the complete object has
passed authentication.

## Playlist and URI contract

The proxy returns verified playlist bytes without textual rewriting. This is
possible because libmpv receives a local URL whose directory mirrors the
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

Restoring validates the entire TSSL document before atomically writing it to
the application-local `tssl` directory. The filename is derived from the root
manifest digest, not from untrusted input. Stored and exported files are set to
owner read/write permissions where supported.

Export resolves the selected remote `.m3u8s`, verifies its digest against the
local store, and writes a normalized TSSL copy to a user-selected path. TSSL is
not encrypted at rest in v1 because the current threat model is remote/cloud
storage, not a compromised local OS account. Exported files contain all media
keys and must be handled as secrets.

## Limits and failure behavior

- TSSL documents are limited to 64 MiB.
- Playlists are limited to 4 MiB.
- Encrypted TS objects are limited to 512 MiB.
- Auxiliary resources are limited to 128 MiB.
- Missing TSSL packages, digest mismatches, unregistered paths, unavailable
  OpenSSL EVP support, network failures, and authentication failures all stop
  playback. There is no unauthenticated or pass-through fallback.

## Standards references

- HLS second edition draft: https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis
- NIST SP 800-38D: https://csrc.nist.gov/pubs/sp/800/38/d/final
- OpenSSL EVP authenticated decryption: https://docs.openssl.org/3.0/man3/EVP_EncryptInit/
