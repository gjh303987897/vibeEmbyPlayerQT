# Local TLS test fixtures

These self-signed certificates and their deliberately public test private key
are used only by a loopback HTTPS server in `AuditRegressionTest`. Never use
them for a deployed service. `valid.pem` has a localhost SAN and expires in
2040; `wrong-host.pem` names `wrong-host.invalid`; `expired.pem` expired in 2001.
All use the same RSA-2048 key so the tests isolate hostname and date checks.

`private-ca-valid.pem`, `private-ca-wrong-host.pem` and `private-ca-expired.pem`
are signed by an isolated test CA whose certificate is deliberately not served
or installed in the trust store. Their leaf private key is the same public test
key above. They reproduce missing-issuer/incomplete-chain failures, including
hostname and expiry failures mixed with trust errors. The valid and wrong-host
fixtures expire in 2040; the expired fixture expired in 2001. Both the WebDAV
login request and playback proxy exercise these certificates.
