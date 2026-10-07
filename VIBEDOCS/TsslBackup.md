# TSSL Backup

CLI backup/restore uses this same backend for WebDAV and S3. Local and downloaded
package imports now share `TsslImportWorkflow` with the Settings page; see
[CLI.md](CLI.md) and [SharedWorkflows.md](SharedWorkflows.md).

The M3U8S manager keeps TSSL key packages in the local encrypted-HLS storage. The
Settings page can copy valid `.tssl` packages to one configured backup target:

- a local folder selected in Settings;
- an existing WebDAV service card; or
- an S3-compatible object-storage endpoint.

Backups are manual. Clicking **Back up TSSL now** immediately reports that package
checking is in progress and prevents duplicate starts. `TsslBackupService::preparePackages`
enumerates and validates the managed packages in a worker using a value copy of
`TsslStore`; neither directory scanning nor JSON parsing runs on the UI thread.
Invalid packages are excluded, and an empty result is reported in Settings.

The selected destination is captured when the operation starts. Local destination
checks and package exports also run in a worker, including checks on slow or
network-mounted folders. Local-folder restores run off the UI thread. The local
packages are never removed after a successful backup.

## Local-folder target

Choose an existing writable folder in Settings and select **Local folder** as
the target. Each valid package is written as a flat `<root-manifest-sha256>.tssl`
file in that folder. Existing files are replaced atomically, and the source
packages in the application TSSL store are not removed.

The **Restore backup** action scans the configured folder for `.tssl` files and
validates each package with the same TSSL parser used by individual imports.
Packages whose root digest already exists locally are skipped and reported as
already existing; malformed or unreadable files are skipped and reported as
failures. A restore never overwrites an existing local package.

## Remote restore

When WebDAV or S3 is selected, **Restore backup** lists the `.tssl` files below
the configured remote folder or object prefix, downloads them one at a time,
and then applies the same local restore validation. Remote packages are never
deleted, and existing local packages are never overwritten.

WebDAV listing uses `PROPFIND` with `Depth: 1`. The response is parsed in a
`QtConcurrent` worker, using only the response bytes and folder URL; DOM parsing
and file filtering do not run on the UI thread. Invalid XML or a response that
is not a WebDAV `multistatus` is reported as a listing error, rather than being
treated as an empty backup folder.

Once listing succeeds, `restoreProgressChanged(completed, total)` reports the
download phase before the first GET and after each completed file. Settings
switches from **Finding TSSL backups** to **Downloading n/total TSSL backups**,
then to local package validation/import. Previously the finding message remained
visible throughout every download, making a slow restore appear stuck at listing.
This progress signal is separate from upload progress, and is used by both remote
targets. Stage transitions and failures are logged without credentials or response
bodies.

Canceling an active listing or download immediately aborts its `QNetworkReply`,
reports cancellation once, and releases the running state so the operation can be
started again. Canceling while the WebDAV XML worker is pending discards its
result when the worker finishes, before any downloads are dispatched. The service
stays busy until that worker result is handled, preventing a canceled listing
from affecting a later restore. Watcher connections use the service as their
QObject context, so destroying the service cannot call it from a late worker result.

Each network request has a 60-second idle timeout, reset when transferred bytes
advance, plus a separate 15-minute total deadline. Both timers stop when the reply
finishes. `TsslBackupNetworkPolicy` can override these values for callers and tests;
the default does not require changes to saved settings or CLI arguments. A completed
cancel is shown as **TSSL restore canceled**, with the controls enabled again.

HTTP 408/429/5xx and selected temporary transport failures retry at most twice,
after 200 and 400 ms. Authentication, malformed content and local-file errors do
not retry. PUT retries reopen an equivalent prepared payload under the same object
name; S3 requests are signed again. Cancellation during backoff finishes without
sending another request. Operation generations prevent old retry timers from
dispatching requests for a new operation.

S3 listing requires a valid `ListBucketResult` XML root and distinguishes malformed
responses from an empty list. XML parsing runs on a worker, with the service busy
until completion. Truncated pages require a continuation token; duplicate tokens,
more than 10,000 pages or a listing exceeding the total timeout are rejected.
Listing responses are bounded to 4 MiB; individual response bodies to 256 MiB.

Protocol and lifecycle references:
[WebDAV PROPFIND (RFC 4918, section 9.1)](https://www.rfc-editor.org/rfc/rfc4918.html#section-9.1),
[Qt reply abort and finished signal](https://doc.qt.io/qt-6/qnetworkreply.html#abort),
[QFutureWatcher completion and cancellation limits](https://doc.qt.io/qt-6/qfuturewatcher.html).

## Credentials

WebDAV uses the password saved for the selected service card. S3 access keys are
stored in QSettings, while the S3 secret key is stored in the platform credential
store under the application secret namespace. Secrets are not returned to QML,
written to logs, or included in request errors.

On platforms where the existing credential store is unavailable, the backup is
rejected rather than falling back to plaintext storage.

## WebDAV target

The configured remote path is appended below the service card's base URL. Each
package is uploaded with an HTTP `PUT` request and its original local filename.
Only `http` and `https` service URLs are accepted, and the existing service-card
TLS trust setting controls self-signed certificate handling.

## S3 target

The implementation uses AWS Signature Version 4 header authentication for a
single `PUT` request per package. The endpoint may be an S3-compatible service;
HTTPS is required except for `localhost` development endpoints. Object keys are
`<prefix>/<original filename>` under the configured bucket. The configured
region, access key, and secret key are required.

Packages larger than 256 MiB are rejected before upload to avoid unbounded memory
use in the network request body. Requests use the idle timeout, total deadline
and bounded retry policy described above.

Each remote upload reads its file and calculates the S3 payload SHA-256 in a worker.
Only one package is prepared/uploaded at a time. The completed payload is handed
back through `QFutureWatcher`; the network manager, replies, and request-body buffers
stay on the service's owning thread. Request signing reuses the same SigV4 helper
as remote restore, with the precomputed payload hash.

Cancellation during upload preparation discards the worker result before sending
a request. The service remains busy until that result has been handled, preventing
an old preparation from affecting a new backup. Worker functions capture paths and
store values, without capturing the service or ViewModel; watcher connections use
the owner's QObject context so closing the application cannot invoke a destroyed
owner. The initial managed-package scan and local copy are not cancelable.

## Verification

`tests/TsslBackupServiceTest.cpp` holds the Qt worker pool to verify that package
preparation and upload reads are asynchronous while UI events continue to be
delivered. A local HTTP endpoint verifies sequential WebDAV uploads and the S3
payload hash. Tests also cover invalid/empty stores, duplicate starts, cancellation
and restart, destruction during preparation, and unreadable/empty/oversized files.
Existing encrypted-HLS format tests cover stable local backup names and exports.

Remote-restore regression tests also verify standard WebDAV listing and sequential
downloads of valid TSSL packages, import into a temporary local store, progress
before downloading, filtering non-TSSL files and collections, background XML
parsing, cancellation of stalled listing/download connections and immediate
restart, malformed XML/HTML/HTTP errors, temporary-file cleanup, and destruction
while parsing is pending. These tests use a local HTTP endpoint and do not require
access to a user's WebDAV account.

## Extension points

`TsslBackupService` is intentionally independent of the settings UI. New remote
targets should provide a `TsslBackupTarget`, validate credentials and endpoint
constraints before starting, and report progress through its existing signal.
