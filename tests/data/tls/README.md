# Local TLS test fixtures

These self-signed certificates and their deliberately public test private key
are used only by a loopback HTTPS server in `AuditRegressionTest`. Never use
them for a deployed service. `valid.pem` has a localhost SAN and expires in
2040; `wrong-host.pem` names `wrong-host.invalid`; `expired.pem` expired in 2001.
All use the same RSA-2048 key so the tests isolate hostname and date checks.
