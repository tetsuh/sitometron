# Walking skeleton (non-normative)

> **Planned, not yet normative:** [Issue #48](https://github.com/tetsuh/sitometron/issues/48) owns
> this spike. Nothing under `spike/` is a contract, a Registry row, or an ADR input. Phases 0B, 1,
> and 2 may adopt, change, or delete any of it.

`sitometron_spike` composes the Phase 0A core (pure reducer and bounded single writer) with real
adapters so that `sitometrond`-shaped behavior can be seen end to end on Linux/WSL:

```text
curl POST /jobs ──> JobDriver ──> JobOrchestrator (core single writer) ──> FileJournal (JSON Lines, fsync)
                       │                 │ HandoffLaunch
                       │                 v
                       └─────────── ProcessRunner (posix_spawn / waitpid)
```

It is built only with `SITOMETRON_BUILD_SPIKE=ON`, is excluded from the Phase 0A qualification set
(clang-tidy scans `apps/`, `src/`, `tests/` only), and adds no vcpkg dependency.

## Build and run

```sh
./bootstrap.sh                      # once: pinned vcpkg tree, configure, build, CTest
cmake -S . -B build/dev-linux -DSITOMETRON_BUILD_SPIKE=ON
cmake --build build/dev-linux --target sitometron_spike

build/dev-linux/spike/sitometron_spike --listen 127.0.0.1:8080 \
    --journal /tmp/sitometron-journal.jsonl --workdir /tmp
```

Options: `--listen HOST:PORT` (port `0` picks an ephemeral port and prints it), `--journal PATH`,
`--workdir DIR` (default working directory for children), `--max-jobs N` (default 32, see
[Findings](#findings-for-phase-0b12)), `--trace-capacity N` (default 4096).

```sh
curl -s localhost:8080/healthz
curl -s -X POST localhost:8080/jobs -H 'Content-Type: application/json' \
     -d '{"executable":"/bin/sh","args":["-c","echo hello; sleep 1"]}'
# {"job_id":"01a0d935-e6bb-7728-a438-650e3ba48ed4"}
curl -s localhost:8080/jobs/01a0d935-e6bb-7728-a438-650e3ba48ed4
curl -s localhost:8080/jobs
curl -s localhost:8080/stats
```

`POST /jobs` returns `202` with the Controller-issued UUIDv7. `GET /jobs/<id>` returns the current
core Job state (`admitted`, `preparing`, `running`, `finalizing`, `succeeded`, `failed`, ...), the
child's pid and exit status, every committed event type so far (`steps`), and the first driver-side
error if any. Unknown ids give `404`; malformed bodies give `400`. `SIGINT`/`SIGTERM` stop the
listener, send `SIGTERM` to live children, let each Job's driver finish its record, and seal the
writer through its shutdown marker.

Smoke test (needs `curl`): `ctest --test-dir build/dev-linux -L spike --output-on-failure`.

## What one Job does

The driver submits the raw lifecycle candidates that the core leaves to "the supervisor". For a
process that starts and exits on its own, the Journal receives 13 records per Job:

| # | event | who produces it in the skeleton |
|---|---|---|
| 1 | `job_created` | core `Create()` with identities from `LocalIdentitySource` |
| 2 | `resources_committed` | driver, one fixed empty allocation (`sha256("{}")` digest) |
| 3 | `worker_launch_intent` | driver, with the generated Worker and launch-operation ids |
| 4 | `worker_launch_observed` | driver, after the writer hands the launch to `ProcessRunner` and `posix_spawnp` returns |
| 5 | `worker_running` | driver, immediately after spawn (there is no Worker protocol yet) |
| 6 | `worker_completed` / `worker_failed` | driver, from `waitpid` (exit 0 vs anything else) |
| 7 | `session_retain_requested` | driver |
| 8 | `session_retained` | driver, after the writer hands the request to `NullSessionRetainer` |
| 9 | `finalization_completed` | driver |
| 10 | `terminal_outcome_committed` | driver (`succeeded` / `failed`), then `RetireWorkerAck` |
| 11 | `process_exit_confirmed` | driver, `process_already_exited` |
| 12 | `resources_released` | driver |
| 13 | `cleanup_status_recorded` | driver, `completed` |

A spawn failure produces `worker_launch_observed{failed}` instead of 4–6 and continues from 7 with
a failed outcome.

## Intentionally missing

Native Windows, TLS, authentication, request limits beyond 64 KiB, Admission, Application Registry,
ResourceProfile/topology, the Worker protocol (`worker_running` is asserted at spawn), cancel and
terminate (the stop ports are no-ops), timeouts (no timer adapter exists, so a hung child never
times out), Journal replay/recovery/pruning (the file is only counted on restart), Sitos, Artifact
REST, Quill logging, packaging, release. Bundle provenance is a placeholder digest.

## Findings for Phase 0B/1/2

Observations gathered while making the skeleton run. They are inputs for the owning design
authorities, not decisions.

1. **The supervisor is an unnamed component.** Of 13 lifecycle events, the core produces one
   (`job_created`) and hands off two (`launch`, `session retain`). Everything else — resource
   commitment, launch intent, session/finalization/terminal sequencing, process-exit confirmation,
   release, cleanup — has to be sequenced by the composition root. `JobDriver` is that sequencer.
   Phase 1/2 should give it a name, an owner, and a contract, or fold parts of it into the core.
2. **There is no production completion notification.** The only way to learn that a submission was
   applied is `WaitUntil(sequence, phase)` (a test barrier that waits for the writer to go idle) plus
   `TakeCompletion(sequence)`. A daemon needs a completion callback or future per ingress sequence.
3. **`Create()` does not return the new identity.** It is read back through the global
   `LastCreated()`, so creation must be serialized by the caller (`create_mutex_`).
4. **Residents are never reclaimed.** `Reserve()` needs `!exists`, and `entity_exists` never becomes
   false after a terminal state, so `max_jobs` is the number of Jobs the daemon can ever accept.
   The trace and ingress-sequence logs are append-only and bounded by `trace_capacity`; the writer
   fails closed when either fills. Memory is reserved up front as
   `2 × max_jobs × trace_capacity` trace records. Phase 0B/1 need resident retirement, a bounded or
   rolling trace, and a documented daemon lifetime model.
5. **Handoff ports run on the writer thread.** `HandoffLaunch` / `HandoffRetainSameIdentity` must
   not block or re-enter ingress. The skeleton uses per-Job mailboxes (`AwaitLaunch`,
   `AwaitRetain`); a production adapter needs the same discipline spelled out in its contract.
6. **No canonical serializer for the logical envelope exists in C++.** The JSON schemas define the
   record, but the mapping from `LogicalJobEvent` to JSON (`FileJournal::ToJson`) had to be written
   by hand here. Phase 0B should own one serializer (and its inverse for replay) next to the schema.
7. **Replay is feasible with the pure reducer.** Because `Apply` is pure, restart recovery can fold
   the Journal file through the reducer to rebuild snapshots. The skeleton does not do it; it only
   counts existing lines.
8. **Per-record `fsync` costs ~4–6 ms on WSL/ext4** (see `recorded_at` deltas), so one Job spends
   60–80 ms in durability alone. Group commit or a dedicated Journal thread is a Phase 0B topic.
9. **Two facts, one process.** For a plain child process, "the Worker completed" and "the process
   exited" are the same observation, but the contract records them as `worker_completed` (before
   terminal outcome) and `process_exit_confirmed` (after). The split is right for a Worker protocol
   that reports completion before exiting; Phase 2 should say what a protocol-less process reports.
10. **Timer generations are armed but nobody fires them.** The writer arms/disarms per-phase timer
    generations and validates `SubmitTimeout`, yet there is no timer port. A timer adapter that
    turns arm effects into real deadlines is required before any timeout can occur.
11. **Global Journal sequence interleaves Jobs.** Two concurrent Jobs share one monotonic sequence
    and their records interleave in the file; per-Job ordering is preserved. Phase 0B should decide
    whether the physical record carries a per-Job sequence as well.
12. **`202 + polling` was enough.** Nothing in the lifecycle needed a streaming or long-poll surface
    for the operator; Phase 1 can start from resource-per-Job JSON and add push later.
