# UpdateService

`UpdateService` owns the GitHub Releases update flow. It keeps release parsing, strict
Stable/Beta/Alpha channel filtering, SemVer comparison, platform asset selection, and
streaming package plus `.sha256` verification out of QML.

`AppViewModel` persists the channel, ETag, and daily-check timestamp through
`SessionRepository`. Downloads are written to the system temporary directory and are
opened with the platform default handler only after the sidecar hash and package size
have been verified.

Release assets must provide a same-name GNU checksum sidecar, for example:

`vibePlayerQT-1.0.2-windows-x86_64-installer.exe.sha256`

