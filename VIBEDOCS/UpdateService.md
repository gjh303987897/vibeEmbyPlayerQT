# UpdateService

`UpdateService` owns the GitHub Releases update flow. It keeps release parsing, strict
Stable/Beta/Alpha channel filtering, SemVer comparison, platform asset selection, and
streaming package plus `.sha256` verification out of QML.

`AppViewModel` persists the channel, ETag, and daily-check timestamp through
`SessionRepository`. Downloads are written to the system temporary directory and are
opened with the platform default handler only after the sidecar hash and package size
have been verified.

Release selection chooses the highest newer channel-compatible version with at
least one package for the current platform and architecture. A release containing
only other platforms or no packages is skipped, so it cannot hide an older usable
update or open an empty download dialog. Packages lacking a checksum remain
visible with the existing warning and disabled download action.

Release assets must provide a same-name GNU checksum sidecar, for example:

`vibePlayerQT-1.0.2-windows-x86_64-installer.exe.sha256`

Checksum parsing accepts whitespace-separated text or binary-marker (`*`)
entries, including manifests containing multiple files. It matches the exact
asset filename and rejects conflicting hashes for that filename. Packages still
must pass the existing size and SHA-256 verification before opening.

