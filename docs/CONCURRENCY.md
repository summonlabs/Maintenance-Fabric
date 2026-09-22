# Concurrency and shutdown audit

## Locking model

There is exactly one mutex in the runtime's decision path: `Controller::mu_`. It
guards the store and every piece of controller state (incarnation, policy,
topology, drain-in-flight set, queued releases, statistics).

Secondary mutexes exist and are all leaf locks, acquired only while `mu_` is *not*
held:

| Owner | Guards | Called from |
| --- | --- | --- |
| `Logger` | stderr formatting | any thread, never while holding `mu_` |
| `LocalDrainPort` | its lease table | the controller's external phase |
| `RemoteDrainPort` | its socket | the controller's external phase |
| `TcpServer` | connection queue and live-connection registry | server threads only |
| `EvidenceStore` / `AuthorityLedger` | nothing | always used under `mu_` |

**Acquisition order is therefore: `mu_` -> (release) -> leaf lock.** No path takes
a leaf lock and then `mu_`, so the lock graph is acyclic.

## Callbacks beneath locks

There are none. The controller never invokes a `DrainPort`, never invokes a
handler, and never calls back into itself while `mu_` is held. The tick is
explicitly two-phase:

1. decide and mutate under the lock;
2. perform external I/O with the lock released;
3. re-acquire and apply, fencing on `(generation, attempt)`.

The scheduler is deliberately a *pure* planner (`plan_tick`) rather than a
callback host precisely so no user-supplied code can run beneath the lock.

## Read-modify-write and re-entrancy

Job mutations are copy-modify-commit: the driver reads an immutable copy, mutates
it, and `Store::commit_job` enforces that the replacement is exactly one revision
newer than what is stored. A writer that lost a race fails with `StaleRevision`
instead of overwriting. Store-level commit helpers stage aggregate changes in a
private copy (`AuthorityLedger`, `EvidenceStore`, `QuarantineTable`) and publish
them only after the journal accepted the record, so a refused append cannot leave
memory ahead of disk.

No runtime path re-enters the controller from beneath the lock: the external
phases of `tick`, and the public API, all acquire `mu_` at their own top level.

## Shutdown

`Controller::shutdown()` sets a stopping flag under the lock and flushes the
journal. Once set:

* `tick` returns `ShuttingDown` from its first pass;
* `propose` and `observe` refuse with `ShuttingDown`;
* no new authority, drain request or state transition is started.

Drain leases are deliberately *not* released on shutdown: the scope may genuinely
be out of service, and the drain service must keep it drained until restoration.
Recovery handles the consequences.

`TcpServer::stop()` follows a strict order:

1. mark stopping under the lock;
2. release the accept call by shutting down and closing the listener;
3. while holding the lock, shut down every registered live connection — the
   workers unregister under the same lock, so a socket can never be destroyed
   while that loop holds a pointer to it;
4. notify the worker condition variable;
5. join the accept thread and every worker **with no lock held**.

A worker parked in a blocking read is released by step 3, so shutdown never waits
for an I/O timeout, and a second `stop()` is a no-op rather than an error or a
hang.

## Bounded surfaces

Everything an untrusted peer can influence is bounded: frame payloads
(`kMaxFrameBytes`), queued connections (`kMaxQueuedRequests`), workers
(`kMaxServerWorkers`), journal records and bytes, snapshot bytes, evidence record
count, reservations, jobs per arbitration, targets per request, dependency lists,
window target lists, retry counters, log message length. Every externally derived
size passes through the checked-arithmetic helpers before it is used, and every
declared count in a decoded record is validated against its bound *before* any
allocation.

## Observed behaviour

`tests/test_concurrency.cpp` exercises four threads mutating and reading the
controller concurrently, six threads leasing and releasing through the drain port,
eight clients against a four-worker server, five start/stop cycles and a stop with
an idle connected client. `tests/test_transport.cpp` additionally starts and
hard-kills real `mfd` and `mf-drain-sim` processes over real sockets.
