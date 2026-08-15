# TLS Certificate Store

## Purpose

`TlsCertificateStore` bridges the operating-system trust store used by Qt with network clients that require a PEM CA bundle, currently libmpv's FFmpeg/OpenSSL HTTPS backend.

The module prevents valid HTTPS media streams from failing when libmpv cannot discover the Windows certificate store by itself.

## Location

- `src/network/TlsCertificateStore.h`
- `src/network/TlsCertificateStore.cpp`

## Public API

`TlsCertificateStore::ensureSystemCaBundle()` returns `std::expected<QString, QString>`:

- success: absolute path to a PEM file containing the operating-system trusted CA certificates
- failure: a diagnostic message suitable for application logging

The function does not contain or persist access tokens, cookies, passwords, server certificates, or other user credentials.

## Behavior

On first use in a process, the module:

1. Reads trusted CA certificates through `QSslConfiguration::systemCaCertificates()`.
2. Encodes each certificate with `QSslCertificate::toPem()`.
3. Writes the combined bundle atomically with `QSaveFile`.
4. Stores the result under `QStandardPaths::CacheLocation/tls/system-ca-bundle.pem`.

Subsequent calls in the same process reuse the first result. A new application process regenerates the file so operating-system trust-store changes are picked up.

## Player Integration

`PlayerController` is still the only module that calls libmpv APIs. Before `mpv_initialize()`, it supplies the generated path through libmpv's `tls-ca-file` option and enables `tls-verify` by default.

For each playback request, `PlayerController::playUrl()` updates the runtime `options/tls-verify` property:

- normal server: certificate verification remains enabled
- explicitly trusted self-signed server: verification is disabled only for that server's playback request

Both foreground media playback and scheduled headless Emby playback pass the saved server certificate policy to `PlayerController`.

Qt network requests use the same saved policy. When it is enabled, the reply ignores
the concrete SSL errors delivered by Qt without opening a blocking confirmation loop.
The setting is persistent per server and the UI labels it as allowing self-signed
certificates; no request-scoped certificate callback is retained by the ViewModel.

## Failure Handling

Failure to export or configure the bundle is logged without exposing the playback URL. Player initialization continues so local files and non-TLS sources remain usable, while subsequent libmpv diagnostics identify any HTTPS certificate failure.

## References

- Qt `QSslConfiguration::systemCaCertificates()`: https://doc.qt.io/qt-6/qsslconfiguration.html#systemCaCertificates
- mpv network TLS options: https://mpv.io/manual/master/#network
