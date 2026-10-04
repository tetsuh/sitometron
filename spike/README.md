# Walking skeleton (non-normative)

> **Planned, not yet normative:** [Issue #48](https://github.com/tetsuh/sitometron/issues/48) owns
> this spike. Nothing under `spike/` is a contract, a Registry row, or an ADR input. Phases 0B, 1,
> and 2 may adopt, change, or delete any of it.

`sitometron_spike` composes the Phase 0A core (pure reducer and bounded single writer) with real
adapters so that `sitometrond`-shaped behavior can be seen end to end on Linux/WSL:

```text
curl POST /jobs ──> JobDriver ──> JobOrchestrator (core single writer) ──> SegmentJournal (sitometron_journal, fdatasync)
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
    --journal /tmp/sitometron-journal --workdir /tmp
```

Options: `--listen HOST:PORT` (loopback addresses only; port `0` picks an ephemeral port and prints
it), `--journal DIR` (the production `SegmentJournal` from `sitometron_journal`, Issue #59: segment
files and exclusive lock; at startup every segment is validated and replayed, Issues #61 and #64),
`--workdir DIR` (default working directory for children), `--max-jobs N` (default 32, see
[Findings](#findings-for-phase-0b12)), `--trace-capacity N` (default 4096), `--segment-limit BYTES`
(default 67108864; the segment size at which the Journal rotates), `--application ID=COMMAND`
(repeatable; registers an Application for the `/v1` prototype below, run as `/bin/sh -c COMMAND`;
`ID` is `[a-z0-9][a-z0-9._:-]*`, at most 128 characters).

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

After a restart every replayed Job is listed with `"recovered": true`. If the previous run was
killed while a Job was still open, the startup line reports it (`unresolved job <id>: admission
closed`), `GET /healthz` returns `503` with `"ready":false` and the unresolved IDs, `POST /jobs`
returns `503`, and nothing is appended to the Journal:

```sh
curl -s -X POST localhost:8080/jobs -d '{"executable":"/bin/sh","args":["-c","sleep 30"]}'
kill -9 <daemon pid>
build/dev-linux/spike/sitometron_spike --listen 127.0.0.1:8080 --journal /tmp/sitometron-journal
# unresolved job <id>: admission closed
# ... (next sequence: N, replayed jobs: 1, unresolved: 1)
curl -s localhost:8080/healthz   # 503 {"ready":false,"status":"not_ready","unresolved":["<id>"],...}
curl -s localhost:8080/jobs/<id> # {"recovered":true,"state":"running","terminal":false,...}
```

With the daemon stopped, two offline subcommands maintain the Journal (Issue #69, `OPS-005`). Both
take the Journal lock, so they refuse with `journal_locked` while a daemon runs, and neither starts
the writer or the HTTP server. `journal prune` moves the longest prefix of sealed segments whose
Jobs are all closed within it into `archive/` inside the Journal directory; replay then skips those
Jobs and the sequence continues unchanged. If a prune is interrupted between moves while a Job spans
the moved segments, the daemon refuses the Journal until `journal prune` runs again; that run lists
the segments already moved as `already archived` and completes the rest. Deleting or restoring
`archive/` is otherwise up to the operator, but not while a prune is interrupted.
`journal quarantine-tail` moves a torn tail, which makes startup refuse with `journal_torn_tail`,
into `<segment>.torn-<offset>` next to the segment and cuts the segment back to its last complete
record. It prints the next sequence, or `sequence exhausted` when the last remaining record carries
the last possible sequence, in which case the daemon still refuses to start. The final line of each
subcommand, its result code, names the Journal directory; the lines before it do not repeat it:

With the default 64 MiB segments a small Journal is one segment, and the highest non-empty segment
always stays, so there is nothing to prune. To see pruning, run the daemon with `--segment-limit 1`
(one record per segment), finish two `/bin/true` Jobs one after the other, and stop it:

```sh
build/dev-linux/spike/sitometron_spike journal prune --journal /tmp/sitometron-journal --dry-run
# would archive journal-00000000000000000001.ndjson   (13 lines, one per record of the first Job)
# would prune job <first id>
# journal_prune_planned: 13 segments, 13 records, 1 Jobs; replay starts at 14 in /tmp/sitometron-journal
build/dev-linux/spike/sitometron_spike journal prune --journal /tmp/sitometron-journal
build/dev-linux/spike/sitometron_spike journal quarantine-tail --journal /tmp/sitometron-journal
# journal_clean: /tmp/sitometron-journal
```

Smoke test (needs `curl`): `ctest --test-dir build/dev-linux -L spike --output-on-failure`.

## External REST v1 prototype

The skeleton also serves a prototype of the External REST v1 Job surface of Accepted
[ADR-0008](../docs/adr/0008-define-the-external-rest-v1-job-surface.md) under `/v1` (Issue #82).
It is not the production adapter and activates none of the `API-*` checks; it exists to run the
contract end to end and to find its gaps. The unversioned routes above stay the skeleton's own.

```sh
build/dev-linux/spike/sitometron_spike --listen 127.0.0.1:8080 --journal /tmp/sitometron-journal \
    --application hello='echo hello; sleep 1' --application broken='exit 3'

curl -s -i -X POST localhost:8080/v1/jobs -H 'Content-Type: application/json' \
     -d '{"application_id":"hello"}'
# HTTP/1.1 202 Accepted
# Location: /v1/jobs/01a0d935-e6bb-7728-a438-650e3ba48ed4
# {"job_id":"01a0d935-...","outcome":null,"state":"admitted","terminal":false}
curl -s localhost:8080/v1/jobs/01a0d935-e6bb-7728-a438-650e3ba48ed4
# {"job_id":"01a0d935-...","outcome":"succeeded","state":"succeeded","terminal":true}
curl -s localhost:8080/v1/jobs      # {"jobs":[...]} in creation order
curl -s localhost:8080/v1/health    # {"status":"ok"}
curl -s localhost:8080/v1/ready     # {"ready":true,"reasons":[]}
curl -s -X POST localhost:8080/v1/jobs -H 'Content-Type: application/json' \
     -d '{"application_id":"hello","executable":"/bin/true"}'
# 422 {"error":{"domain":"request","code":"validation_failed","message":"...","details":{}}}
```

Served: `POST /v1/jobs`, `GET /v1/jobs/{job_id}`, `GET /v1/jobs`, `GET /v1/health`, and
`GET /v1/ready`, with the error envelope and status table of ADR-0008 Section 5. The client names a
registered `application_id` only; the command comes from `--application`. Not served yet:
`POST /v1/jobs/{job_id}/cancel` (it answers `404` `route_not_found`), because the driver cannot
stop a Job yet.

Gaps between ADR-0008 and what a listener has to decide, found while building this:

1. **HTTP-level refusals have no row.** A request that is not valid HTTP, and one that is not
   received in time, are refused before any route is known. The prototype answers `400`
   `request`/`malformed_request` and `408` `request`/`request_timeout`.
2. **The stated check order cannot hold for an oversized request.** ADR-0008 orders route, method,
   and media type before size, but a listener bounds the request while reading it, so an oversized
   request is `413` whatever its route.
3. **`already_pending` has no status.** A second creation during identity generation gets this
   ingress result. The prototype serializes creation so that it never occurs; a production adapter
   must do the same or the ADR needs a row.
4. **Query strings are not mentioned.** The prototype answers every `/v1` target that has a query
   with `404` `route_not_found`.
5. **A `POST /v1/jobs` without a body** has no media type to check; it is `400` `malformed_json`.
6. **`HEAD` and `OPTIONS`** are answered like any other method that a route does not allow.
7. **`details` is always empty** here. ADR-0008 allows fields in it but names none, so a client
   cannot yet learn, for example, which field failed validation.
8. **`terminal` is true before cleanup is recorded.** The terminal outcome is committed before
   process-exit confirmation, release, and cleanup, so a client can see a terminal Job whose
   resident slot work is still finishing. ADR-0008 Section 3 allows this; it is worth stating.

9. **The characters of `application_id` are not defined.** ADR-0008 Section 4 gives only a length
   of 1 to 128, Section 1 says identifiers are lowercase ASCII, and the core records the
   Application identity as a stable identifier (`[A-Za-z0-9][A-Za-z0-9._:-]*`). The prototype
   registers only `[a-z0-9][a-z0-9._:-]*`; a request with any other text is `unknown_application`.

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
times out), recovery of Jobs left unresolved by a crash (they are replayed and shown, but the daemon
then stays not ready with admission closed; Phase 2 owns their resolution), online Journal pruning, Sitos, Artifact
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
   **Resolved** by `AwaitCompletion(sequence)` (Issue #80), which waits for that one sequence; the
   driver uses it for every step and for the shutdown marker.
3. **`Create()` does not return the new identity.** It is read back through the global
   `LastCreated()`, so creation must be serialized by the caller (`create_mutex_`). **Resolved**
   for the identity by `CreateJob()` (Issue #80). The driver still serializes creation, because a
   second creation during identity generation is refused with `already_pending`.
4. **Residents are never reclaimed.** `Reserve()` needs `!exists`, and `entity_exists` never becomes
   false after a terminal state, so `max_jobs` is the number of Jobs the daemon can ever accept.
   The trace and ingress-sequence logs are append-only and bounded by `trace_capacity`; the writer
   fails closed when either fills. Memory is reserved up front as
   `2 × max_jobs × trace_capacity` trace records. Phase 0B/1 need resident retirement, a bounded or
   rolling trace, and a documented daemon lifetime model. **Partly addressed** across restarts:
   offline pruning (Issue #69) removes closed Jobs from replay, so a restarted daemon regains their
   slots. Retirement inside one run is tracked in Issue #73 (Gate #50 finding H7).
5. **Handoff ports run on the writer thread.** `HandoffLaunch` / `HandoffRetainSameIdentity` must
   not block or re-enter ingress. The skeleton uses per-Job mailboxes (`AwaitLaunch`,
   `AwaitRetain`); a production adapter needs the same discipline spelled out in its contract.
6. **No canonical serializer for the logical envelope exists in C++.** The JSON schemas define the
   record, but the mapping from `LogicalJobEvent` to JSON had to be written by hand here. Phase 0B
   should own one serializer (and its inverse for replay) next to the schema. **Resolved** by
   Issue #57 (canonical record codec) and Issue #59 (the spike now writes through it).
7. **Replay is feasible with the pure reducer.** Because `Apply` is pure, restart recovery can fold
   the Journal file through the reducer to rebuild snapshots. The skeleton does not do it; it only
   reads the last sequence so that new records continue the numbering, and Jobs from a previous
   run are invisible to the API after a restart. **Resolved** by `ReplayJournal` (Issue #61) and
   writer seeding (Issue #64): the spike replays every segment at startup, lists replayed Jobs as
   `"recovered": true`, and after `kill -9` during a Job answers `/healthz` and `POST /jobs` with
   503 for the whole run (resolving such a Job is Phase 2 scope, Issue #35).
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
