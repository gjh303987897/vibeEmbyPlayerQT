# TSSL backup ZIP format

`TsslBackupArchive` is a portable file-only module shared by desktop and CLI
backup workflows. It never imports packages, performs network requests, reads
credentials, or changes a `TsslStore`. Its synchronous API must run on a worker.

## Public API

- `create(files, destination, canceled, progress)` validates packages, compresses
  one ZIP, and atomically publishes it with `QSaveFile`. It returns the number
  of packages. Existing destinations are rejected.
- `extract(archive, destinationDirectory, canceled, progress)` validates the
  entire archive, stages its package files, and then publishes them without
  overwriting files. It returns absolute paths only after all validation succeeds.
- `discover(directory)` returns readable regular `.tssl` files and recognized
  `tssl-backup-*.zip` archives. Other ZIP files and symbolic links are ignored.
- `isArchiveName(name)` accepts only a filename matching the backup convention,
  not a path. `newFileName()` combines a UTC timestamp with a UUID so creating a
  backup does not replace a previous snapshot.

Both operations return `std::expected<..., QString>`. The optional atomic cancel
flag is checked at package boundaries and ZIP I/O callbacks. Progress callbacks
receive `(completedPackages, totalPackages)` on the worker thread. They must not
directly access UI objects.

## Archive layout

Version 1 is a standard unencrypted ZIP using stored or Deflate entries. New
backups use Deflate level 3 to balance CPU cost and size. ZIP data is written and
read through Qt file callbacks rather than loading the archive into memory or
passing encoded filesystem paths to C stdio. Unicode destination paths therefore
work on Windows as well as on other Qt platforms.

Entries are flat:

```text
manifest.json
<64 lowercase hexadecimal root digest>.tssl
<64 lowercase hexadecimal root digest>.tssl
```

The `.tssl` bytes are preserved exactly. The manifest contains:

```json
{
  "format": "TSSL-BACKUP",
  "version": 1,
  "count": 1,
  "createdAt": "2026-10-13T00:00:00.000Z",
  "packages": [
    {
      "file": "<root digest>.tssl",
      "rootManifestSha256": "<root digest>",
      "sha256": "<SHA-256 of the exact TSSL file bytes>",
      "size": 12345
    }
  ]
}
```

This changes the backup transport container, not the TSSL v2/v3/v4 package format
or video encryption. New restore workflows also discover legacy loose `.tssl`
backups. Older application versions can restore a ZIP after it is manually
extracted; each extracted TSSL is still an ordinary importable package.

## Validation and resource limits

The module enforces these bounds before publishing restored package paths:

| Resource | Limit |
| --- | --- |
| One TSSL package | 64 MiB, matching `TsslPackage::parse` |
| Compressed ZIP file | 2 GiB |
| Total expanded ZIP entries | 2 GiB |
| Package count | 10,000 plus one manifest entry |
| Manifest JSON | 4 MiB |
| Central directory | 8 MiB |

The ZIP end record is inspected before initializing miniz so its central
directory allocation cannot grow with an arbitrary archive size. ZIP64 and
multi-disk layouts are unnecessary within these limits and are rejected.
Compression holds only one bounded TSSL document in memory for its existing
parser; it never buffers the complete ZIP. Extraction streams each entry into a
private temporary directory, reads at most one bounded package for validation,
and verifies ZIP CRC, local-versus-central headers, manifest count, filename,
root digest, byte size, SHA-256, and the existing TSSL parser.

Nested paths, traversal paths, links, devices, directories, encrypted ZIP entries,
unsupported compression methods, unexpected entries, and duplicate filenames
are rejected. A bad entry invalidates the complete archive; importing valid
entries from an incomplete archive is left to explicit individual-file imports.
Cancellation or failure removes staging data and any files newly published by
that extraction, while preserving existing destination files. Source TSSL files
and the input ZIP are never removed.

The ZIP itself is not password-encrypted. It contains the same sensitive key
material as legacy `.tssl` backups, so destination access permissions matter.
New local files request owner read/write permissions through Qt.

## Dependency and verification

The module uses the small portable MIT [miniz 3.1.0 upstream release](https://github.com/richgel999/miniz/tree/3.1.0).
Vendored source provenance and SHA-256 values are recorded in
[`third_party/miniz/PROVENANCE.md`](../third_party/miniz/PROVENANCE.md).
The callbacks and ZIP operations follow the upstream
[`miniz_zip.h` API documentation](https://github.com/richgel999/miniz/blob/3.1.0/miniz_zip.h).

`TsslBackupArchiveTest` covers byte-preserving multi-package round trips,
Unicode paths, legacy/new discovery, invalid and duplicate input packages,
an independently produced .NET standard-ZIP fixture,
traversal/link/unexpected ZIP entries, corrupt archives and manifest mismatches,
metadata limits before extraction, cancellation cleanup, and no-overwrite
publication with rollback. Service-level tests cover the network workflow that
uses this module; see [TsslBackup.md](TsslBackup.md).
