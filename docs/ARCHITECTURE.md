# Architecture

## Layers

| Layer | Headers | Responsibility |
| --- | --- | --- |
| core | `mf/core/*` | checked arithmetic, strong ids, byte codecs, hashing, clock, bounded logging |
| domain | `mf/domain/*` | identities, topology, policy, evidence, windows, drain contract, authority, job model, lifecycle rules |
| engine | `mf/engine/*` | readiness view, capacity impact projection, precondition evaluation, arbitration, tick planning, explanations |
| store | `mf/store/*` | record codecs, write-ahead journal, durable state |
| runtime | `mf/runtime/*` | fencing helpers, the controller that drives the lifecycle |
| transport | `mf/transport/*` | framed protocol, sockets, bounded server, blocking client |
| drain | `mf/drain/*` | `DrainPort` implementations: an in-process reference and a remote client |
| config | `mf/config/*` | strict text loading of topology and policy |

Dependencies run strictly downwards: core -> domain -> engine -> store/runtime ->
transport/drain -> apps. Nothing in `domain` or `engine` performs I/O, acquires a
lock, or reads a clock of its own; time, evidence and policy are always injected.

## Decisions are pure functions

Three functions carry the safety argument:

* `compute_impact(topology, policy, readiness, baseline, planned)` projects exactly
  how many serving units a removal set would consume, per redundancy pool, per
  correlated failure domain, per availability contract, and reports the first
  governing violation with the rule that produced it.
* `evaluate_preconditions(...)` turns that projection plus evidence freshness,
  dependency state and window admissibility into a `PreconditionSet` with a
  digest, the policy revision and the topology revision it consumed.
* `arbitrate(...)` orders candidate jobs and grants authority against the union of
  live reservations.

All three are deterministic and side-effect free apart from the ledger they are
given, which makes them directly testable and is what the property suite exploits.

## The controller

`Controller` owns every piece of mutable runtime state behind a single mutex. Its
tick is a bounded loop of passes:

1. Under the lock: expire authority leases, build a readiness view from fresh
   evidence, and drive each job one step.
2. Collect the external actions the step produced (drain request/release).
3. Outside the lock: perform those actions.
4. Under the lock: apply the outcomes, fencing each one against the job's current
   generation and attempt.

At most eight passes run per tick, so one call can carry a job from `validated` to
`in-maintenance` without ever holding the lock across an external call.

## Authority and the joint-safety rule

A reservation records the job, generation, attempt, owning incarnation, the exact
removal set and its lease. Granting is refused when:

* any planned target is already covered by another live reservation (two jobs may
  never own the same target);
* any planned target lacks fresh evidence (the runtime cannot see what it is
  about to remove);
* the projected union with every live reservation would break redundancy,
  correlated-domain, headroom or contract policy;
* the concurrency bound from `max_concurrent_jobs` is reached.

Because the ledger is the single serialization point and the projection is
computed against live reservations rather than against job intent, two planners
cannot independently consume the same tolerance budget.

## Persistence and recovery

Each mutation is written as one transaction frame:

```
+--------+---------+--------+--------+-----------+--------------+---------------+
| type   | flags   | seq    | length | payload   | payload crc  | header crc    |
| u16    | u16     | u64    | u32    | ...       | u32          | u32 (first 20)|
+--------+---------+--------+--------+-----------+--------------+---------------+
```

Recovery walks frames from a 12-byte file header and stops at the first frame
whose header checksum, length bound, record type, sequence number or payload
checksum fails, then truncates the file at the last good boundary. Topology and
policy records are strict (a runtime that cannot decode them cannot know what it
protects); per-entity records are discarded and counted. Compaction rewrites the
whole state into a temporary file, fsyncs it and atomically renames it over the
journal.

On startup the controller claims a new incarnation: the boot epoch is incremented
and durably recorded before anything else happens. Reconciliation then:

* revokes every reservation owned by a fenced incarnation;
* for jobs that never removed service: clears the attempt, releases the drain
  lease and blocks the job back onto `prerequisites`;
* for jobs that removed service: mints a **new attempt**, re-issues the
  reservation under the new incarnation so the capacity stays reserved, and blocks
  the job so the scheduler resumes it at `verifying`.

Every attempt, drain lease and evidence record from the previous process is
therefore fenced, and stale evidence is not silently fresh again.

## Boundaries

* **Drain Fabric** is external. The runtime holds a lease and requires
  attempt-bound confirmation before removing service. `LocalDrainPort` is an
  in-process simulator used by tests, examples and benchmarks;
  `RemoteDrainPort` speaks the framed protocol to a real service, and
  `mf-drain-sim` is a reference implementation of that foreign side.
* **Maintenance execution** is external. The runtime consumes completion reports
  bound to the exact attempt, and never claims to perform or roll back an upgrade.
* **Health, topology and workload evidence** are external observations with a
  source, a revision and a time-to-live.
