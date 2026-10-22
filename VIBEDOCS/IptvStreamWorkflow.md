# IPTV stream metadata workflow

`src/services/iptv/IptvStreamWorkflow.h/.cpp` contains side-effect-free mutations
shared by the desktop ViewModel and CLI playback host. It does not access the
network, database or libmpv. `IptvStreamSelector` remains the single ranking and
stable stream-ID implementation; `IptvProbe` remains the bounded asynchronous
network/sample probe.

- `applyProbe(channel, sourceUrl, result)` merges one original route's probe.
  Fresh actual playback or failure information wins over a late probe. Detailed
  results can fill missing variant dimensions without clearing a failure time.
- `applyPlayback(channel, sourceUrl, playbackUrl, resolution, codec, bitrate)`
  accepts positive dimensions, updates route/channel metadata and clears failure
  state on the selected HLS variant. Empty metrics cannot replace known quality.
- `markFailed(channel, selection)` marks the selected child variant failed, or
  the complete route when it is a direct stream. Cache expiry/ranking stay in
  the selector.
- `rows(channel, preference, currentId)` returns metadata and stable IDs without
  source URLs. CLI output cannot disclose signed stream addresses.

The desktop retains its existing queue, 150 ms pacing, coalesced background
database writer and playback lifecycle. Only the existing probe-result and
actual-playback mutations delegate to this module. The CLI uses the same
mutations with its independent host; see [CliDesktopParity.md](CliDesktopParity.md).

No new probe processes or dependencies are introduced. Existing six-hour
successful-quality and 60-second failed/incomplete cache windows still apply.
An unavailable measurement has a concrete state rather than fabricated quality.

Regression coverage comes from the desktop state tests, multi-source selector/
probe tests and CLI tests using actual libmpv fallback and quality correction.
