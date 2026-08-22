# TSSL Backup

The M3U8S manager keeps TSSL key packages in the local encrypted-HLS storage. The
Settings page can copy valid `.tssl` packages to one configured remote target:

- an existing WebDAV service card; or
- an S3-compatible object-storage endpoint.

Backups are manual and upload one package at a time. The UI reports preparation,
per-file progress, completion, cancellation, and failures. The local packages
are never removed after a successful backup.

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
use in the network request body. Network requests have a 60-second timeout.

## Extension points

`TsslBackupService` is intentionally independent of the settings UI. New remote
targets should provide a `TsslBackupTarget`, validate credentials and endpoint
constraints before starting, and report progress through its existing signal.
