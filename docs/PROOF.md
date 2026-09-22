# Proof surfaces

Every claim in this repository is labelled REAL, SYNTHETIC or UNSUPPORTED.

## REAL

* **Build.** Release and Debug builds with MSVC `/W4 /WX` (or GCC/Clang with
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion ... -Werror`) complete
  with **zero first-party warnings**.
* **Tests.** `ctest` runs the full suite to completion with no watchdog and no
  timeout property. A hang would be a defect to diagnose, not something hidden.
* **Persistence.** The journal is a real file on disk with per-frame header and
  payload CRC32-C checksums, sequence numbers and transactional records.
  Truncation, byte corruption, header corruption, unsupported layout revisions and
  short files are exercised against actual files, and recovery truncates the
  journal exactly where integrity first fails.
* **Sockets.** `TcpServer`, `TcpClient`, `RemoteDrainPort` and `mfd` speak a real
  framed TCP protocol over loopback with real deadlines, real connection
  backpressure (a bounded accept queue that refuses rather than grows) and real
  shutdown ordering.
* **Independent processes.** `tests/test_transport.cpp` spawns `mf-drain-sim` and
  `mfd` as separate OS processes, drives them over the wire, hard-kills them with
  `TerminateProcess`/`SIGKILL`, restarts them against the same journal, and
  verifies that the previous incarnation's attempt is fenced and that the
  out-of-service scope stays reserved.
* **Concurrency.** Real threads contending on the controller, the drain port and
  the server; real start/stop cycles; real join on shutdown.
* **Determinism.** Arbitration and explanations are asserted byte-identical across
  repeated evaluation, and the property suite asserts explanation digests are
  stable across a randomized run.

## SYNTHETIC

* **`LocalDrainPort`** is an in-process *simulator of a foreign system*. It
  implements the drain contract with injectable failures (refused requests,
  dropped leases, owner fencing) so the boundary can be exercised deterministically.
  It is never presented as a drain service.
* **`mf-drain-sim`** is a reference implementation of the *foreign* side of the
  boundary, written in this repository so the wire protocol can be proven end to
  end. It performs no maintenance decision making and is not part of the runtime.
* **`--health-provider static-healthy`** in `mfd` is a lab convenience: it
  publishes a healthy observation for every topology target on a fixed cadence.
  Production deployments publish real observations from real probes.
* **Failure injection** (drain failure, link loss, maintenance-process death,
  controller restart, lease loss) is produced by the runtime's own test controls,
  not by breaking real hardware.

## UNSUPPORTED

No claim is made, and no test asserts, anything about:

* RDMA, NVLink, multi-GPU, SmartNIC or any accelerator fabric;
* any specific switch, router, optics or firmware vendor, or any vendor CLI,
  NETCONF/RESTCONF/gNMI endpoint or upgrade procedure;
* multi-node consensus, Raft/Paxos, quorum storage, or a distributed controller —
  the runtime is a single-writer controller with a durable journal, and
  "distributed" here means exactly two things that are proven: a real
  out-of-process drain service over TCP, and replicated *clients* of one daemon;
* performance at datacentre scale. The benchmark reports throughput of the
  operations it actually completes on the machine it runs on;
* non-Windows behaviour. The POSIX code paths are written and compile-guarded but
  were not exercised on this machine, which is Windows/MSVC.

## Domain proof obligations and where they are discharged

| Obligation | Evidence |
| --- | --- |
| Maintenance cannot begin until required prerequisites are authoritatively satisfied | `controller.full_lifecycle_*`, `preconditions.*`, `controller.start_is_blocked_*` |
| Overlapping maintenance cannot violate failure-domain/redundancy policy | `arbitrator.overlapping_jobs_cannot_both_remove_correlated_capacity`, `controller.overlapping_jobs_cannot_both_start`, `controller.conflicting_jobs_on_the_same_scope_are_refused` |
| Restoration requires explicit verification | `controller.full_lifecycle_reaches_complete_only_after_verification`, `controller.unverified_targets_never_return_to_service`, `controller.failed_restoration_check_*` |
| Old attempts cannot complete a newer job generation | `controller.stale_attempt_and_generation_are_fenced`, `controller.rearm_bumps_the_generation_and_discards_old_tokens`, `transport.daemon_is_driven_over_real_sockets_and_fenced_across_a_kill` |
| Stale topology/workload evidence cannot authorize start | `preconditions.stale_evidence_blocks_admission`, `controller.start_is_blocked_while_health_evidence_is_stale`, `store.evidence_is_not_fresh_after_a_restart`, `adversarial.evidence_replays_and_generation_confusion` |
| N+1 / N+2 as generic policy | `impact.n_plus_one_blocks_when_tolerance_is_already_consumed`, `impact.n_plus_two_is_expressible_as_generic_policy`, `mf_example_custom_policy` |
| Spontaneous link loss after approval, before start | `controller.spontaneous_link_loss_after_approval_blocks_the_start` |
| Workload arrival / contract enforcement | `impact.capacity_headroom_and_contracts`, `preconditions.*` |
| Drain failure | `controller.drain_failure_blocks_after_the_policy_bound`, `transport.drain_service_injected_failure_is_visible_over_the_wire` |
| Maintenance process death | `controller.maintenance_process_death_leaves_the_job_in_maintenance`, `controller.window_close_can_abort_and_quarantine` |
| Controller restart | `controller.restart_requires_fresh_verification_and_keeps_capacity_reserved`, `controller.restart_before_service_removal_releases_everything`, `property.randomized_restarts_never_resume_removed_service` |
| Unsafe jobs remain blocked | `controller.unmaintainable_targets_are_a_hard_block`, `property.randomized_lifecycle_preserves_invariants` |
| Adversarial input | `adversarial.*` (fuzzed configuration, frames, RPC bodies and records) |
| Crash safety | `property.journal_truncation_at_every_offset_is_survivable`, `property.byte_corruption_never_produces_inconsistent_state` |

## Repository closure proof

The release checklist that produced `v1.0.0` covers: a manual tree inspection,
`git status --porcelain` cleanliness, removal of build debris and generated
artefacts, a fresh clone built from committed sources only, install plus a
downstream `find_package` consumer that builds and runs against the installed
artefact, and verification that the remote resolves to the closure commit.
## Defects found and fixed during hardening

The validation campaign found and repaired these material defects; each one now
has a regression test.

1. **Journal replay was impossible after a crash.** A guard meant for the append
   path was placed inside `Journal::replay`, so a journal holding committed frames
   refused to replay and therefore refused every subsequent append. Regression:
   `journal.append_requires_replay`, `store.state_survives_a_restart`.
2. **Topology records were written child-first.** Records were emitted in identity
   order, but a child target is only decodable once its parent exists, so a
   persisted topology could not be read back. Regression:
   `records.malformed_payloads_are_rejected`, `store.state_survives_a_restart`.
3. **Terminal jobs leaked their authority reservation.** Completion released
   resources only when no service had been removed, so a completed job kept
   holding capacity out of service forever. Regression:
   `controller.full_lifecycle_reaches_complete_only_after_verification`,
   `property.randomized_lifecycle_preserves_invariants`.
4. **Completed jobs still counted as removed capacity.** The impact baseline
   tested the sticky `service_removed` flag instead of the current lifecycle
   stage, so every completed job permanently consumed its pool's redundancy
   budget. Regression: `benchmark job-lifecycle throughput`,
   `controller.overlapping_jobs_cannot_both_start`.
5. **Job plans were treated as removed capacity.** A job that was merely planning
   contributed its scope to another job's baseline, so two planners could deadlock
   each other. Regression: `controller.conflicting_jobs_on_the_same_scope_are_refused`.
6. **The arbitrator could grant two jobs the same target.** Capacity arithmetic
   alone cannot see a double claim, because a target present in both the baseline
   and the plan is still one unit out of service. Regression:
   `arbitrator.overlapping_jobs_cannot_both_remove_correlated_capacity`.
7. **A drained job kept believing it held a lease.** A successful release erased
   the lease record but not the job's reference, so the job re-requested the
   release on every pass and never restored. Regression:
   `controller.full_lifecycle_reaches_complete_only_after_verification`,
   `transport.daemon_is_driven_over_real_sockets_and_fenced_across_a_kill`.
8. **Reconciliation re-issued authority with an empty attempt.** The recovery path
   cleared the attempt and then tried to reserve with it, so a restarted daemon
   could not keep an out-of-service scope reserved. Regression:
   `controller.restart_requires_fresh_verification_and_keeps_capacity_reserved`.
9. **The control-plane quorum probe never checked freshness.** The evidence query
   left its clock and incarnation unset, so a valid quorum observation was always
   reported as fenced. Regression:
   `preconditions.control_plane_requires_quorum_evidence`.
10. **Remote drain requests were sent without their RPC envelope.** The client sent
    a bare body, so every request to a real drain service failed to decode.
    Regression: `transport.remote_drain_port_talks_to_an_independent_process`.
11. **A denied arbitration left the job silently waiting.** The refusal reason was
    recorded in the report but not on the job, so an unsafe job sat in
    `prerequisites` instead of being visibly blocked. Regression:
    `controller.start_is_blocked_by_redundancy_policy`.
12. **Sub-second health republishing was rejected as a replay.** `mfd` derived
    evidence revisions from the wall clock, so a cadence faster than one second
    produced duplicate revisions. Regression:
    `transport.daemon_refuses_a_second_scope_on_the_same_capacity`.
13. **Soft-blocked jobs churned state every pass.** A blocked job was resumed in
    the same tick that blocked it, oscillating between `blocked` and
    `prerequisites`. Regression:
    `controller.drain_failure_blocks_after_the_policy_bound`.
14. **The FNV-1a 64 offset basis was mistyped.** Content digests were still
    deterministic, but they were not FNV-1a. Regression:
    `hash.deterministic_values`.
15. **A benchmark policy that violated the runtime's own evidence bound was
    silently ignored**, which made the benchmark measure an unintended
    configuration. The benchmark now fails loudly on a rejected fixture.
