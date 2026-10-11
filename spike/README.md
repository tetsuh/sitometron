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

The optional Issue #98 Sitos build is separate from the walking skeleton. Keep the Python virtual
environment outside the checkout (here `~/opt/venvs/sitometron-spike`, with `sitos==0.1.3` and
`eclipse-zenoh==1.9.0`), so the working tree stays clean:

```sh
cmake -S . -B build/spike-sitos -G Ninja \
    -DSITOMETRON_BUILD_SPIKE=ON \
    -DSITOMETRON_BUILD_SPIKE_SITOS=ON \
    -DSITOMETRON_SPIKE_PYTHON="$HOME/opt/venvs/sitometron-spike/bin/python" \
    -DCMAKE_PREFIX_PATH="$HOME/opt/sitos-0.1.3" \
    -Dzenohc_ROOT="$HOME/opt/zenoh-c-1.9.0" \
    -DZENOHC_ROOT="$HOME/opt/zenoh-c-1.9.0"
cmake --build build/spike-sitos --target sitometron_spike
LD_LIBRARY_PATH="$HOME/opt/sitos-0.1.3/lib:$HOME/opt/zenoh-c-1.9.0/lib" \
  ctest --test-dir build/spike-sitos -R spike_sitos_session_smoke --output-on-failure
```

`SITOMETRON_BUILD_SPIKE_SITOS` is OFF by default. When ON, the driver hosts one Sitos
`StorageNode`, creates one Session per Job, and passes `SITOS_PREFIX`, `SITOS_SESSION_ID`, and a
client configuration to the child. The opt-in smoke test uses `scripts/session_worker.py` and
`scripts/buffer_reader.py`; it covers success, non-zero exit, cancel, parameter round trips, raw
buffer reading, and post-close buffer absence. For every post-close absence check, the reader first
queries the seeded `<prefix>/base/input` parameter through the same Zenoh session and requires a
reply; only then does zero replies for the durable route prove that `CloseSession` removed it
while the StorageNode was reachable. A no-node reader invocation fails with `SITOS_READER_ERROR`
instead of accepting an unobservable absence. For cancel, it waits for that Job's own
`SITOS_READER_OK` marker before sending the cancel, so the post-close absence check proves cleanup
of a published value.

Options: `--listen HOST:PORT` (loopback addresses only; port `0` picks an ephemeral port and prints
it), `--journal DIR` (the production `SegmentJournal` from `sitometron_journal`, Issue #59: segment
files and exclusive lock; at startup every segment is validated and replayed, Issues #61 and #64),
`--workdir DIR` (default working directory for children), `--max-jobs N` (default 32, see
[Findings](#findings-for-phase-0b12)), `--trace-capacity N` (default 4096), `--segment-limit BYTES`
(default 67108864; the segment size at which the Journal rotates), `--application ID=COMMAND`
(repeatable; registers an Application for the `/v1` prototype below, run as `/bin/sh -c COMMAND`;
`ID` is `[a-z0-9][a-z0-9._:-]*`, at most 128 characters), `--cancel-principal NAME` (default
`local-operator`; the principal recorded for a `/v1` cancel, 1 to 256 visible ASCII characters).

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

## Issue #98: Sitos Session spike

This section records the non-normative result of the opt-in build. It is a finding log, not a Sitos
contract or a Phase 4 decision.

### What worked with Sitos 0.1.3 as it is

- The C++ package built from tag `v0.1.3` with one in-process `StorageNode`, an in-memory base
  engine, and an in-memory durable-buffer engine factory.
- A Job's canonical lowercase UUIDv7 was accepted as the Sitos Session ID. The driver created the
  Session with both durable and ephemeral capabilities before spawning the child and closed it after
  the terminal outcome.
- With an explicit local TCP client configuration, the worker's `ParamCache` read the seeded base
  value and `ParamStore` acknowledged and read back the Session output value.
- A raw Zenoh reader read the durable bytes while the Session was active. After `CloseSession`, the
  same reader received no durable value after its control query received the seeded
  `<prefix>/base/input` reply. The smoke test waits for each Job's own `SITOS_READER_OK` marker
  before terminal cleanup and observed this for success, non-zero exit, and cancel paths; a
  separate no-node case fails rather than treating an unreachable node as absence.
- The worker's raw fallback put both durable and ephemeral byte routes through the documented
  `zenoh/bytes` wire shape. The durable route was visible before close and absent after close.

### Usage pitfalls and observed behavior

- The one-argument `CreateSession(sid)` overload leaves durable and ephemeral buffer routes
  disabled. The spike therefore uses `CreateSession(sid, SessionOptions{durable_buffers=true,
  ephemeral_buffers=true})`; a disabled-route push is not a usable buffer path.
- Default Zenoh discovery did not connect the independently spawned Python wheel client to the C++
  node in this environment. A Checkpoint 3b `zenoh.scout` run saw no Hello messages. The cause is
  unknown because firewall state was not readable without root. The C++ host therefore listens on
  local TCP port `27400` and passes an explicit client configuration to the worker and raw reader.
  Concurrent opt-in daemons using this spike port are not supported.
- The Sitos 0.1.3 Python API has no endpoint option other than the complete `zenoh_config_json`.
  `src/transport/zenoh_transport.cpp` at v0.1.3 uses `z_config_default()` when no JSON is supplied;
  it does not read `ZENOH_CONFIG`. Any caller JSON disables the Fence profile by design, so a
  worker that needs explicit endpoints cannot use `BufferPublisher` Fence.
- Sitos 0.1.3 rejects a caller-supplied Zenoh configuration for `BufferPublisher` with
  `ValueError: Transport does not support Fence`. The reason is that the normal transport factory
  disables the Fence capability whenever JSON configuration is supplied. The worker records this
  exact result and uses raw `zenoh/bytes` puts as a visible fallback; consequently this run proves
  the buffer routes and cleanup, but does not claim a successful `BufferPublisher` Fence.
- The Sitos Python documentation warns that independently opened `ParamCache`, `ParamStore`, and
  `BufferPublisher` sessions in one Python process are unsupported. The sequential Cache/Store
  pairing worked with the explicit configuration here; the BufferPublisher limitation above
  prevented a complete same-process three-client qualification. This remains a Phase 4 finding.
- Durable buffers are owned by the in-memory Session engine and disappear when `CloseSession`
  returns. A reader must run before terminal cleanup closes the Session; a post-close reader is
  expected to receive no reply. A future durable product path needs an explicit retention/reader
  decision.

### Candidate Sitos changes, with evidence

1. Add a supported raw durable-buffer reader API or a documented reader helper. Evidence: the
   acceptance reader has to issue raw `get` on
   `<prefix>/buffers/<sid>/durable/<key>` because Sitos 0.1.3 exposes no buffer reader.
2. Provide a way to choose endpoints while retaining the verified Fence profile, or expose a
   supported explicit-config Fence mode. Evidence: `src/transport/zenoh_transport.cpp` at v0.1.3
   disables the profile for caller JSON; the local explicit TCP configuration makes parameters
   reachable but produces the observed `Transport does not support Fence` refusal.
3. Define durable-buffer lifetime and crash/cancel semantics for the adapter. Evidence: normal
   non-zero and cancel Jobs close the Session and remove in-memory durable values; a reader cannot
   inspect them after close, and a crash can leave an unresolved Job requiring startup recovery.
4. Document the required StorageNode startup order and a collision-safe endpoint/configuration
   handoff. Evidence: the C++ node must be ready before the worker's explicit client opens, while
   the driver must close every Session before stopping the node.
5. Keep the C++ and wheel zenoh-c pins aligned. Evidence: the C++ source/cache and the wheel NOTICE
   both identify zenoh-c `1.9.0`; the standalone C++ SONAME and wheel-repaired SONAME differ only
   because wheel repair renames the bundled file.

### Consumer provisioning findings

| Step | Command family | Elapsed | Result |
|---|---|---:|---|
| Project bootstrap after process-local environment cleanup | `env -u VCPKG_DEFAULT_TRIPLET -u VCPKG_FEATURE_FLAGS ./bootstrap.sh` | 51.41 s | 69/69 tests passed |
| Sitos source checkout | `git clone --branch v0.1.3 --depth 1 --no-tags ...` | 0.75 s | commit `59f4dfb24bdde7e1d766f41216b169e5194442dd` |
| zenoh-c archive download | official 1.9.0 standalone ZIP | 1.02 s | downloaded; SHA-256 `29f6a0dedc7ada0be007058e706df64ff26e62ae56f6efc2b098132c431c263d` |
| zenoh-c extraction | `unzip` into `~/opt/zenoh-c-1.9.0` | 0.30 s | headers, static library, and shared library present |
| Sitos C++ configure/build/install | Release, Zenoh ON, RocksDB/tests/Python OFF | 0.11 s / 4.97 s / 0.00 s | installed under `~/opt/sitos-0.1.3` |
| Python venv | `python3.12 -m venv .venv-spike` | 1.66 s | Python 3.12.3, pip 24.0 |
| Published Sitos wheel | `pip install --only-binary=:all: sitos==0.1.3` | 3.31 s | CPython 3.12 manylinux wheel; NumPy 2.5.4 |
| Raw Python Zenoh wheel | `pip install --only-binary=:all: eclipse-zenoh==1.9.0` | 1.43 s | installed |

The ZIP download was **not checksum-verified before extraction**. Phase 4 provisioning must pin and
verify the recorded SHA-256 before opening the archive. Sitos remains outside the vcpkg manifest.

### Inputs kept separate for the Phase 4 design

- Provision the pinned Sitos source and zenoh-c tree outside the Sitometron repository, then consume
  the installed CMake package with `find_package(sitos CONFIG REQUIRED)` only under the opt-in spike
  option.
- Provision the published CPython 3.12 wheel in an isolated environment; the raw reader additionally
  needs `eclipse-zenoh==1.9.0`.
- Make the zenoh-c archive checksum a repository-owned Phase 4 pin and verify it before extraction.
- Decide whether Phase 4 owns a buffer reader, a durable retention boundary, and an explicit-config
  Fence transport capability. This spike does not change Sitos or promote the raw fallback to a
  product adapter.
- Define the `CreateSession` failure path. With the test-only, opt-in
  `SITOMETRON_SPIKE_FAIL_CREATE_SESSION=1` environment variable, the smoke test observed an admitted
  Job reaching `failed` through `worker_launch_observed{failed}` and the normal finalization,
  terminal, release, and cleanup events; no child ran, the refusal reason remained in the driver
  error, and shutdown reported zero open Sessions. The variable is unset by default.

## External REST v1 prototype

The skeleton also serves a prototype of the External REST v1 Job surface of Accepted
[ADR-0008](../docs/adr/0008-define-the-external-rest-v1-job-surface.md) under `/v1` (Issues #82
and #84).
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
curl -s -X POST localhost:8080/v1/jobs/01a0d935-e6bb-7728-a438-650e3ba48ed4/cancel
# 202 {"job_id":"01a0d935-...","outcome":null,"state":"stopping","terminal":false}
# 409 {"error":{"domain":"job","code":"command_not_allowed_in_state",...}} once it is terminal
```

Served: `POST /v1/jobs`, `GET /v1/jobs/{job_id}`, `GET /v1/jobs`,
`POST /v1/jobs/{job_id}/cancel`, `GET /v1/health`, and `GET /v1/ready`, with the error envelope and
status table of ADR-0008 Section 5. The client names a registered `application_id` only; the
command comes from `--application`.

Cancel records `cancel_accepted` under one configured principal (`--cancel-principal NAME`,
default `local-operator`). What follows depends on whether the launch intent is recorded:

- Before `worker_launch_intent` there is no process. The Job goes straight to `finalizing` and
  ends `cancelled` without a signal.
- From `worker_launch_intent` on, the Job is `stopping`. `SIGTERM` is sent to the Job's child
  process, or delivered as soon as the child is spawned if it does not exist yet, and the Job
  ends `cancelled` when the child has exited.

Limitations of the skeleton's stop (Issue #84):

- There is no timer adapter, so the cooperative-stop timeout never fires and no forced stop
  follows. A child that ignores `SIGTERM` stays `stopping` until it exits by itself or the daemon
  shuts down.
- The signal goes to the child process only, not to processes it started. A command that should
  stop cleanly handles `SIGTERM` itself or replaces the shell with `exec`.

Gaps between ADR-0008 and what a listener has to decide, found while building this:

1. **HTTP-level refusals have no row.** A request whose request line is not
   `METHOD /target HTTP/1.0` or `HTTP/1.1`, and one that is not received in time, are refused
   before any route is known. (The skeleton's listener checks the request-line form and the
   `Content-Length` value and size; it does not validate HTTP syntax in general.) The prototype answers `400`
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
   registers only `[a-z0-9][a-z0-9._:-]*`; a request with any other text of 1 to 128 characters
   is `unknown_application`, and an empty or longer string is `validation_failed`.
10. **A JSON library is not a JSON validator.** The parser the skeleton uses reads a raw NUL byte
    as the end of its input and accepts the text before it, so a body could carry a valid object,
    a NUL, and anything after it. It also accepts duplicate keys. The prototype refuses both
    before parsing counts as success; the production adapter needs the same tests whatever library
    it uses. The unversioned `/jobs` route does not have these checks.
11. **A cancel body that is not an empty object has no code.** ADR-0008 Section 3 says that a
    body, if present, must be an empty JSON object, but names no error for the other cases. The
    prototype treats a present body like the body of a creation: `415` without the JSON media
    type, `400` `malformed_json`, and `422` `validation_failed` for anything but `{}`.
12. **The `202` of a cancel does not always show `stopping`.** A cancel accepted before the
    launch intent is recorded moves the Job straight to `finalizing`, and a Job can leave
    `stopping` before the response is built. A client can rely only on the status; the `state` is whatever is committed
    when the resource is read.
13. **The Job that keeps the daemon not ready cannot be cancelled.** A cancel is a normal ingress
    input, so while admission is closed by an unresolved Job, a cancel of that Job is `503`
    `not_ready` like any other request. ADR-0008 Section 5 says this for an unknown Job; it holds
    for the unresolved Job too, which is worth stating for operators.
14. **A cancel overtakes the supervisor between its steps.** After `cancel_accepted` the driver's
    next candidate is refused, and what it owes next depends on where the cancel landed. The
    smoke test cancels right after creation and has reached these landing points (which ones a
    run reaches is timing): before the launch intent (the Job is already `finalizing`; nothing
    was launched, only the release remains), after the launch intent or after
    `worker_launch_observed` (the Job is `stopping` and no Worker will report, so the driver
    submits `process_exit_confirmed` once the child has exited), and while running (the Worker's
    report ends `stopping`). A cancel before `resources_committed` takes the same code path as
    the first case with nothing to release; no run has reached it. The skeleton reads the
    snapshot to decide. A production supervisor needs this table written down (see finding 1 of
    the next list).

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
a failed outcome. A Sitos `CreateSession` failure follows the same failed-launch path before any
child is spawned; it records the driver error and continues through terminal cleanup without an
open Sitos Session.

## Intentionally missing

Native Windows, TLS, authentication, request limits beyond 64 KiB, Admission, Application Registry,
ResourceProfile/topology, the Worker protocol (`worker_running` is asserted at spawn), terminate
and the forced stop (that port is a no-op; cancel is served on `/v1` only), timeouts (no timer adapter exists, so a hung child never
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
