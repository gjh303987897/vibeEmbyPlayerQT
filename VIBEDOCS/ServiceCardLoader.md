# ServiceCardLoader

`src/services/management/ServiceCardLoader` reads service-card snapshots for the
desktop service list, privacy-management list and scheduled Emby source picker.
The CLI can continue using the synchronous read-only repository on its own
terminal thread; this loader has no QML dependency.

`refresh()` queues a background read using Qt Concurrent. The worker constructs
and destroys a unique, read-only `SessionRepository` connection on its own thread.
It does not migrate the database, prune history, fetch credentials or capture the
loader or ViewModel. A missing database produces an empty snapshot; read errors
emit `failed(message)` on the caller's thread.

Refreshes in the same event turn coalesce. During an in-flight read, further
requests advance a generation; the obsolete result is discarded and one fresh
read follows. Only the latest result emits `ready(cards)`. There is at most one
worker per loader. Destruction disconnects delivery; an existing worker can finish
using only its captured path and repository, with the existing bounded SQLite
busy timeout.

Snapshots contain all enabled cards and a saved-session availability flag, never
Token contents. `AppViewModel` derives the visible and scheduled-source models
using its current privacy mode. Locking privacy removes private cards from those
models immediately, before a database read can finish. Privacy management keeps
its existing full-card metadata view. Ordinary cards remain visible during
refresh; a latest read failure reports an error and retains their previous data.

`AuditRegressionTest` holds an exclusive database lock while requesting repeated
refreshes, verifies that timer heartbeats continue, releases the lock and updates
a card, and checks that only the latest snapshot is published on the owner thread.
Missing and malformed database tests verify empty/error results and absence of
read-side database creation or modification.

Related storage rule: editing an existing card preserves `last_used_at`. New cards
receive their creation timestamp. Successfully saving a login session updates the
timestamp in the same transaction as the card and session; a failed session write
rolls back both configuration and time changes.

`AppViewModelStateTest` uses actual QML bindings to verify the independent
`loadingServiceCardIdChanged` notification when replacing a pending card or
leaving the services page. Another case unlocks privacy, requests a background
snapshot, then locks again; no stale private result can reappear. Its optional
database path propagates to service activation, history and local-root workers
as well as the snapshot loader, keeping test profiles isolated.
