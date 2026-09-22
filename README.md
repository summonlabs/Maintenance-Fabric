# Maintenance Fabric

Vendor-neutral maintenance eligibility and lifecycle runtime.

Maintenance Fabric decides **whether maintenance may start, what prerequisites it
requires, and when a resource may return to normal service**. It sits in front of
whatever performs the work — a firmware tool, a configuration pipeline, a human
with a console — and refuses to let that work begin while the topology,
redundancy policy, workload contracts, failure domains or evidence freshness say
it is unsafe.

It does **not** implement upgrades, software rollout or generic configuration
management. It consumes *drain capability* and *planned maintenance actions* as
external boundaries.

```
        maintenance request
                |
                v
   +---------------------------+        asks for a drain lease        +-----------------+
   |    Maintenance Fabric     | <---------------------------------> |  Drain Fabric   |
   |  eligibility + lifecycle  |                                    |  (external)     |
   +---------------------------+                                    +-----------------+
        |            ^
        |            |  health / topology / workload evidence (aged, sourced)
        v            |
   authority over capacity held out of service
```

## What is implemented

**Typed domain model.** Jobs, generations, attempts, targets, failure domains,
pools, windows, authorities, drain leases, evidence and controller incarnations
are strongly typed value objects with explicit validity and a total order. Ids of
different kinds never compare, convert or mix.

**Request model.** A maintenance request names links, ports, switches, racks,
pods, sites or control-plane components. A request for a container targets its
whole subtree: a rack request expands to every target beneath it.

**Preconditions.** Admission is evaluated against topology redundancy (N+K as
generic per-pool policy, never a hardcoded vendor convention), correlated
failure-domain limits, capacity headroom, workload/availability contracts, active
drains, conflicting maintenance and evidence freshness. Every precondition result
is `satisfied`, `violated` or `pending`, with a reason code and detail.

**Lifecycle.** `proposed -> validated -> prerequisites -> ready -> in-maintenance
-> verifying -> restoring -> complete`, plus `blocked`, `cancelled` and
`failed`. Backward movement is legal only while no service has been removed.
Once a job removes service it may only move toward verification and restoration,
and cancellation is refused.

**Maintenance authority.** The right to hold capacity out of service is a
reservation granted by a deterministic arbitrator against the *union* of every
live reservation. Two jobs that are each individually admissible but jointly
unsafe cannot both be granted. Authority is incarnation-owned: a restarted
process never inherits or releases authority it did not grant.

**Drain boundary.** Before service is removed the runtime asks an external drain
service for a lease covering the whole removal set, and requires fresh,
attempt-bound confirmation before starting. Drain failure blocks the job after a
policy-bounded number of attempts.

**Windows as policy, not timers.** A window gates admission (start requires lead
time before it closes). It never prevents restoration. If a window closes while
an attempt is running, the configured `on_close` policy decides: let the active
step finish and record an overrun, sanction the overrun explicitly, or stop
granting authority and fail the job into quarantine if the attempt does not
report completion within the abort grace.

**Evidence freshness.** Every operational fact is an aged observation with a
revision, a time-to-live, a source and the incarnation that observed it. Evidence
that merely survived a restart is *not* fresh: volatile observations must be
re-observed by the running incarnation. A decision always cites the policy
revision, topology revision, readiness digest and precondition digest it used.

**Post-maintenance verification.** Restoration requires fresh evidence that every
target in the removal set is serving, no negative restoration report bound to the
current attempt, and no open quarantine. The verdict is written as evidence
before the job may complete.

**Restart-safe persistence.** An append-only journal with per-frame header and
payload checksums, per-frame sequence numbers and transactional records.
Recovery is conservative: the first frame that fails any check ends the replay and
the file is truncated exactly there. Structural records (topology, policy) that
cannot be decoded are fatal; per-entity records are discarded and counted.
Reconciliation fences every attempt and token issued by the previous process and
drives jobs that removed service into mandatory verification — while keeping
their capacity reserved.

**Deterministic scheduler and explanations.** Arbitration orders candidates by
(policy tier rank, request time, job id); the same inputs always produce the same
decisions, and every decision records its ordering key and rejection reason.
Explanations are byte-identical for identical inputs and carry a content digest.

## Layout

```
include/mf/          public headers (core, domain, engine, store, runtime, transport, drain, config)
src/                 implementation of the same layers
apps/mfd             the daemon: controller + store + framed RPC
apps/mfctl           operator CLI (local journal or remote daemon)
apps/drain_sim       mf-drain-sim: reference implementation of the FOREIGN drain service
examples/            basic flow, generic N+K policy, restart recovery
bench/               completed-work benchmarks
tests/               unit, domain, engine, store, controller, property, adversarial, concurrency, transport
docs/                architecture, concurrency audit and proof-surface notes
```

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix <prefix>
```

Requirements: CMake 3.25+, a C++20 compiler (MSVC 19.3x with `/W4 /WX`, or
GCC/Clang with the strict warning set in `CMakeLists.txt`), and nothing else — the
runtime has no third-party dependencies. On Windows the TCP transport links
`ws2_32`; the TCP transport is part of the library on every supported platform.

The installed package exports `MaintenanceFabric::mf`:

```cmake
find_package(MaintenanceFabric 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE MaintenanceFabric::mf)
```

## Using it

```cpp
#include "mf/drain/local.hpp"
#include "mf/runtime/controller.hpp"

mf::ManualClock clock(1750000000LL * mf::kNanosPerSecond);
auto drain = std::make_shared<mf::LocalDrainPort>();   // stands in for the real drain service

mf::ControllerOptions options;
options.store.journal_path = "state/journal.mfj";
mf::RecoveryReport recovery;
auto controller = mf::Controller::open(options, drain, clock, recovery).value();

controller->set_topology(std::move(topology));
controller->set_policy(std::move(policy));

mf::MaintenanceRequest request;
request.title = "replace optics";
request.targets = {mf::TargetId{mf::TargetKind::Link, 1}};
request.requestor = mf::requestor_from_name("oncall");

const mf::JobId job = controller->propose(request, "oncall").value();
controller->approve(job, "oncall");
controller->tick(clock.now());            // admission, authority, drain, start
// ... the external executor performs the work ...
controller->report_completion(job, attempt, true, "optics replaced");
// ... verification, restoration and completion follow on later ticks ...
```

### Command line

```
mfctl --store state/journal.mfj --topology fabric.mftopo --policy fabric.mfpolicy \
      propose --title "replace optics" --target link:1 --priority 1
mfctl --store state/journal.mfj approve job:1
mfctl --store state/journal.mfj start job:1
mfctl --store state/journal.mfj status job:1
mfctl --store state/journal.mfj explain job:1
mfctl --store state/journal.mfj conflicts
```

`mfctl` in local mode owns the journal directly and steps a deterministic clock
(`--now`, `--advance`). Point it at `--addr host:port` to drive a running `mfd`
instead; both modes perform the same operations and report the same error codes.

### Configuration format

Topology and policy are strict, line-oriented text: one record per line,
`key=value` tokens, `#` comments. An unknown record kind, an unknown key, a
repeated key or a malformed value is rejected with its line number — a typo can
never be silently ignored.

```
domain kind=rack id=1 name=rack-a correlated=true
target kind=link id=1 name=lag-1 parent=switch:1 domains=rack:1,power:1
pool id=1 name=uplink members=link:1,link:3,link:5,link:7

redundancy pool=1 min_viable=1 tolerated_losses=1 name=n-plus-1
domain_limit domain=rack:1 max_out=1 name=one-per-rack
headroom pool=1 reserve=1 name=keep-a-spare
contract id=1 name=web members=link:1,link:3 min_available=1
window id=1 name=nightly opens=2026-07-01T01:00:00Z closes=2026-07-01T05:00:00Z \
       min_lead=10m on_close=finish-active-step abort_grace=2m
tier index=0 name=emergency
setting key=max_concurrent_jobs value=8
```

N+K is written as `min_viable=N tolerated_losses=K`; nothing about a vendor's
convention is assumed anywhere in the runtime.

## Validation

The suite runs plainly — no test uses a timeout or watchdog. It covers unit,
domain, engine, persistence, controller, randomized property, adversarial,
concurrency and independent-process transport surfaces. See
`docs/PROOF.md` for the REAL / SYNTHETIC / UNSUPPORTED proof boundaries and
`docs/CONCURRENCY.md` for the locking and shutdown audit.

```
ctest --test-dir build --output-on-failure
build/bench/mf_bench --jobs=400 --records=20000
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
