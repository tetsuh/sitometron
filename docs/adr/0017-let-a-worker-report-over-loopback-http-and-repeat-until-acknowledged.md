# ADR-0017: Let a Worker report over loopback HTTP and repeat until acknowledged

## Status

Proposed — 2026-11-22

## Context

ADR-0016 starts each Job as one local process and leaves the Worker protocol open: "The Worker
protocol (Issue #127) adds the variables a Worker needs to report, in the same `SITOMETRON_`
namespace", and it "decides how its adapter holds or retries a report until the committed snapshot
shows the launch observed". Until that protocol exists, "a process without the Worker protocol
cannot succeed".

The Accepted contracts already fix most of what a Worker protocol may do:

- ADR-0012 Section 1 assigns `worker_running`, `worker_completed`, and `worker_failed` to "the
  Worker protocol (Phase 2)". The reducer decides them (ADR-0002): `worker_running` carries only the
  Worker identity and is accepted only in `preparing` after the launch is observed;
  `worker_completed` and `worker_failed` carry the Worker identity and an event sequence of at least
  1, and in `finalizing` or a terminal state they are normalized to `late_worker_event`.
- ADR-0002 fixes the acknowledgment: "Worker delivery deduplication and sequence validation occur at
  the Worker-protocol boundary. A terminal Worker event binds an acknowledgment obligation to its
  Worker ID and event sequence. After the matching terminal outcome is synced, the Phase 2 adapter
  sends that idempotent ACK and retries until delivery or confirmed process exit. Terminal commit
  does not erase retry identity; the first accepted `process_exit_confirmed` clears the binding."
- ADR-0003 classifies `worker_running` as a normal input and the two terminal reports as critical
  inputs behind one Worker gate per Job: "the Worker stop-and-wait gate retains at most one
  unacknowledged event sequence and its exact semantic payload against its ACK obligation. An exact
  same-sequence retransmission coalesces only while that ACK obligation is retained and receives no
  new ingress sequence, reducer turn, Journal record, or ACK authorization. A pending or previously
  authorized ACK may be retried by that protocol state; `coalesced_pending` itself never authorizes
  an ACK." A different report while one is retained is answered `already_pending`.
- ADR-0005 adds no acknowledgment port: it excludes "a response, ACK, readiness, resource,
  finalization, or generic callback port" and any "retry, or Worker-delivery API" from the core's
  ports.
- ADR-0014 keeps a Job with an acknowledgment-binding Worker event resident until its process exit
  is confirmed, and says of a retired Job that "a Worker fact, a process-exit observation, a release
  fact, or a cleanup fact for a retired Job fails to acquire its lease and is dropped by its source,
  with no ingress result, no reducer turn, no Journal record, and no failure latch".
- ADR-0004 states that "UUIDs are identifiers and must never be used as Worker bearer credentials"
  and leaves the bearer token to a separate design.
- ADR-0008 serves External REST v1 "on a loopback address only" and trusts every local process
  that can reach that listener: "Every local process that can reach the loopback listener is
  trusted as the operator". ADR-0015 Section 6 keeps its I/O model, one I/O thread with
  synchronous completion waits (owner decision A of 2026-10-24), and assigns the question "whether
  the Worker listener shares that thread" to this decision.

An HTTP server cannot observe that a response it wrote was received. A protocol in which the daemon
decides that an acknowledgment "was delivered" therefore needs either a second exchange or a rule
that does not depend on delivery.

The owner decided on 2026-11-22, as recorded in Issue #127: the acknowledgment is driven by the
Worker, which repeats its terminal report until the daemon answers that it is acknowledged; the
Worker protocol has its own loopback listener and I/O thread; a first report that arrives before the
launch observation is committed is answered "not yet, send again"; Phase 2 has no Worker credential;
a Worker exits promptly after its acknowledgment; and v1 has no heartbeat or progress report. On
2026-11-23 the owner decided that `--worker-listen` is optional and that a terminal report for a Job
whose process exit is already confirmed is refused and not recorded.

## Decision

We will define Worker HTTP v1: two loopback routes on a listener of their own, through which a
Worker reports that it is running and reports its terminal outcome, and in which the Worker repeats
the terminal report until the daemon's answer says it is acknowledged. Sections 1 to 9 are normative
for every implementation; anything they do not state is not part of v1.

### 1. Transport, listener, and trust

- The daemon serves Worker HTTP v1 as HTTP/1.1 on a loopback address of its own, given by the
  startup option `--worker-listen ADDRESS:PORT`. It refuses to start with a Worker listen address
  that is not loopback, with the errors and the refusal status of its other startup options. The
  option follows the parsing rules of `--listen`: a repeated option, a missing or empty value, and
  a malformed address are startup errors, and port 0 asks for a free port. The same address with
  the same nonzero port as `--listen` is a startup error. The option is optional: without it the
  daemon has no Worker listener, sets no Worker endpoint (Section 2), and every process is the
  protocol-less process of ADR-0016 Section 2.
- The Worker listener is a second listener of the HTTP adapter with its own I/O thread. It shares no
  thread with the External REST v1 listener, so a report never waits for that listener's thread,
  whatever its handler is waiting for. Both listeners submit to the one writer (ADR-0003): a report
  that needs a writer turn waits for the turns admitted before it, a creation's among them. Each of
  the two listeners keeps the model of owner decision A of 2026-10-24: one I/O thread, and a handler
  waits synchronously for the completion of the one writer turn its request submitted. No handler
  waits for anything else (Section 4).
- The daemon does not authenticate a connection in this slice. Every local process that can reach
  the Worker listener is trusted to report for any Job whose identities it knows, as ADR-0008
  Section 1 trusts every local process on the External REST listener. The identities in a report
  are matched against the committed Job only to refuse a misrouted report; that match is not
  authentication (ADR-0004). Restricting who can reach the listener is the deployment's
  responsibility until Worker bearer tokens are designed.
- Every path of this contract starts with `/worker/v1`. The External REST listener serves no
  `/worker/v1` path and the Worker listener serves no `/v1` path; each answers the other's paths
  `404` `route_not_found`.
- Request and response bodies, the message-syntax rules, the limits, and the refusals before a
  route is known are those of External REST v1 (ADR-0008 Section 1, ADR-0009 Sections 1, 2, 4, 5,
  6, 7, and 8), applied to this listener with the same configured bounds. The compatibility rules
  of ADR-0008 Section 2 apply to `/worker/v1` as they do to `/v1`.

### 2. How a Worker learns its endpoint and identities

The runner (ADR-0016 Section 1) already gives every process `SITOMETRON_JOB_ID`,
`SITOMETRON_WORKER_ID`, and `SITOMETRON_LAUNCH_OPERATION_ID`. When the daemon has a Worker listener,
the runner adds one variable, overriding any inherited value:

| Variable | Value |
|---|---|
| `SITOMETRON_WORKER_ENDPOINT` | `http://ADDRESS:PORT`, the address and the bound port of the Worker listener, with no path and no trailing slash; an IPv6 address is written in brackets, as a URI authority requires (`http://[::1]:PORT`) |

A Worker forms a request URL by appending a path of Section 3 to that value. When the daemon has no
Worker listener the runner adds nothing: the process inherits the daemon's environment as ADR-0016
Section 1 says, so an operator who starts the daemon without a Worker listener must not leave the
variable set.

The composition binds the Worker listener's address, so that its port is known, before it builds the
runner, and gives the endpoint to the runner's producer as a value of its own, next to the launch
specifications. The listener starts accepting connections after the runner is built and before the
External REST listener opens (Section 11).

### 3. Routes

Both routes take `POST` with a JSON object body. `{job_id}` is a canonical lowercase UUIDv7.
`worker_id` is a canonical lowercase UUIDv4. An unknown field, a missing field, a wrong type, and a
value out of range are `422` `validation_failed` (ADR-0008 Section 5); the strict JSON rules of
ADR-0009 Section 8 apply before that. A report without a body is `400` `malformed_json`, as
ADR-0009 Section 5 says of a creation.

| Route | Request body | Answers |
|---|---|---|
| `POST /worker/v1/jobs/{job_id}/running` | `{"worker_id": "..."}` | `204` with no body: the Job is `running` for this Worker |
| `POST /worker/v1/jobs/{job_id}/outcome` | `{"worker_id": "...", "outcome": "completed"}` | `202` `{"acknowledged": false}`: recorded, not yet acknowledged; `200` `{"acknowledged": true}`: acknowledged |

- `outcome` is `completed` or `failed`. It selects `worker_completed` or `worker_failed`.
- A request carries no event sequence. ADR-0002 requires one of at least 1 in the Worker event and
  leaves "sequence validation" to "the Worker-protocol boundary"; the adapter assigns it from the
  outcome: 1 for `completed` and 2 for `failed`. The committed binding of Section 4 holds the
  Worker identity and that sequence, so it identifies the recorded outcome. Two requests are the
  same report exactly when their `job_id`, `worker_id`, and `outcome` are equal.
- A report carries nothing else: no exit status, message, progress, or result. Results and
  Artifacts are later phases.

### 4. The acknowledgment exchange

The acknowledgment of a terminal report is the answer `200` `{"acknowledged": true}` to that
report. The daemon never opens a connection to a Worker, and no request is held open while an
acknowledgment is not yet authorized.

ADR-0002 keeps the acknowledgment obligation in the committed snapshot: a terminal Worker event
"binds an acknowledgment obligation to its Worker ID and event sequence", "Terminal commit does not
erase retry identity", and "the first accepted `process_exit_confirmed` clears the binding". The
adapter answers from that binding, so the answer does not depend on which daemon lifetime recorded
the report.

1. The Worker sends its terminal report. If the committed snapshot holds no binding (Section 5),
   the adapter submits the report to the writer as the matching critical input and waits for that
   turn (Section 8). That turn records the report (`worker_completed` or `worker_failed`), moves
   the Job to `finalizing`, and binds the obligation; it does not authorize the acknowledgment.
2. ADR-0002 authorizes the acknowledgment in a later turn, the one that commits the matching
   `terminal_outcome_committed`. The supervisor drives the Job from `finalizing` to that record
   through its own inputs (ADR-0012), each in a writer turn of its own. ADR-0003 makes the ACK
   effect of that turn the authorization: "An ADR-0002 ACK effect is itself ACK authorization at
   its declared effect-list position". The writer installs a turn's snapshot before it dispatches
   the turn's effects, so a terminal snapshot alone is not yet the authorization. The adapter
   therefore asks the core whether the acknowledgment of the Job's binding is authorized
   (Section 8) and answers `202` `{"acknowledged": false}` while the binding names this report and
   it is not, and `200` `{"acknowledged": true}` once it is. The first answer is `202` unless the
   authorization had already happened when the adapter asked.
3. The Worker repeats the same report until it receives `200` `{"acknowledged": true}` or it gives
   up and exits (Section 9). A repetition finds the binding in the committed snapshot and is
   answered `202` or `200` by the same rule, without a submission (Section 5, row 5). It therefore
   gets no ingress sequence, reducer turn, or Journal record, and it waits for nothing.
4. The binding, and with it the answer, lasts until the Job's process exit is confirmed. The daemon
   does not end it when it writes a `200`: a response that was written may not have arrived, and
   the next repetition must get the same answer.
5. After a restart the replayed snapshot holds the same binding, so a Worker that outlived the
   daemon is still answered. Replay "dispatches no post-sync effect" and "sends no acknowledgment"
   (ADR-0006 Section 6), so no ACK effect exists in the new daemon. By the owner's decision of
   2026-11-24, the durable `terminal_outcome_committed` record is then the authorization: for a
   Job that replay restored, the acknowledgment is authorized exactly when the restored snapshot is
   terminal and holds the binding. The ACK effect follows from that record without any further
   decision, so the record decides what the effect would have decided. This includes a daemon that
   stopped after the record was durable and before the effect was dispatched: the Worker was
   answered `202` before the stop and is answered `200` after the restart, which is the same
   progress it would have seen without the stop.
6. If the process exit is confirmed after a report's turn and before the adapter reads the
   snapshot again, the binding is gone and the answer is the `409` `process_exited` of Section 5.
   The retry obligation is not extended past the exit. A report whose turn came after the exit is
   a different case, decided in Section 5: the reducer records it as a `late_worker_event`.

This is ADR-0002's "the Phase 2 adapter sends that idempotent ACK and retries until delivery or
confirmed process exit" for a protocol in which the Worker is the client: the adapter sends the
acknowledgment in every answer to a repetition of the report, it does so for as long as the Worker
repeats, and confirmed process exit ends the obligation. Delivery is observed by the Worker, which
stops repeating. ADR-0003, as ADR-0014 Section 6 words it, names three causes that retire the
Worker gate: "Successful ACK, confirmed process exit, or retirement under ADR-0014 retires the
Worker gate and identity". v1 never asserts the first, because the daemon cannot observe it; the
other two apply as written. Because a repetition is answered before ingress, ADR-0003's coalescing
of an exact retransmission is not reached by this adapter; the gate still admits the one terminal
report and refuses any other while it is retained.

A terminal report with the other `outcome` while a binding exists is answered `409`
`report_conflict` from the snapshot and is not submitted. This holds for as long as the binding
exists, also after the acknowledgment and after a restart.

### 5. Answers decided from the committed snapshot

A process can start and report before the runner's `worker_launch_observed{started}` is committed,
a Worker can repeat a report that was already recorded, and a report can arrive for a Job whose
process is gone. The adapter decides these cases from the committed snapshot, after the request
checks and before it submits anything. The first row that matches decides:

| # | The committed snapshot shows | Report | Answer |
|---|---|---|---|
| 1 | no such resident Job | any | `404` `job_not_found` |
| 2 | another Worker bound to the Job, or none yet | any | `409` `worker_mismatch` |
| 3 | the Job's process exit confirmed | any | `409` `process_exited` |
| 4 | the Job's launch not yet observed | any | `503` `launch_not_observed` |
| 5 | an acknowledgment binding for this report's `outcome` | terminal | `200` if its acknowledgment is authorized, otherwise `202` (Section 4) |
| 6 | an acknowledgment binding for the other `outcome` | terminal | `409` `report_conflict` |
| 7 | the Job `running` | `running` | `204` |
| 8 | the Job `preparing` (its launch observed) | `completed` | `409` `running_not_reported` |

A report that matches no row is submitted, and the writer's answer applies (Sections 6 and 8). In
every such case the launch is observed and the exit is not confirmed. They are: a `running` for a
Job that is `preparing`, which the reducer accepts and which is answered `204` when its turn
commits; a `running` in any later state, which the reducer rejects; and a first terminal report,
which is a `failed` for a Job that is `preparing` or either outcome for a Job that is `running` or
`stopping`. A Job with a bound Worker and no confirmed exit reaches `finalizing` only through a
recorded terminal report (ADR-0002), so a terminal report for a Job in `finalizing` or a terminal
state always meets row 3, 5, or 6.

- The rows submit nothing and depend only on the snapshot, so they hold whether or not admission is
  open and whichever daemon lifetime recorded the Job: a repeated `running` or a repeated terminal
  report is answered the same after admission has closed and after a restart.
- Row 3 is the owner's decision of 2026-11-23. Once the exit is confirmed the Worker is gone, so a
  report can only come from another process, and it is refused without a record.
- Row 4 covers every first report in every state in which the launch is not yet observed: a Job
  that is `preparing`, and a Job that a cancel, a terminate, or the preparation timeout moved to
  `stopping` before the runner's observation (ADR-0016 Section 2). The reducer would accept a
  `failed` there, and in `stopping` a `completed` too, and it would then reject the runner's
  observation, so the adapter submits none of them. The adapter holds no request until the launch
  is observed and arms no timer; the Worker's repetition is the only retry. If the launch is never
  observed as started, the runner's exit report ends the Job and row 3 answers.
- Row 8 guards `preparing` only, where the reducer rejects `completed`. In `stopping` the reducer
  accepts either outcome whether or not `running` was reported, and the report is submitted; that
  a Worker sends `completed` only after `running` is its obligation under Section 9, not a rule
  the adapter enforces there.

The snapshot can be overtaken before a submission is admitted. The writer and the reducer then
decide, nothing is latched, and the adapter reads the snapshot again after a terminal report's turn
to answer by the rows above:

- a `running` whose Job was retired in between goes through normal admission as for an unknown Job
  (it is a normal input without a gate, ADR-0003): admitted, the reducer rejects it as
  `job_not_found` under ADR-0002's absent-entity rule, as ADR-0014 Section 2 says of a terminate
  for a retired Job, and the answer is `404`; refused by admission, the answer is the `503` of
  that refusal;
- a terminal report whose Job's exit was confirmed in between, and whose lease is still granted, is
  admitted and normalized by the reducer to the `late_worker_event` of ADR-0002. It is recorded as
  an audit fact and changes no outcome, and ADR-0002 authorizes its acknowledgment with the record.
  The adapter learns from the completion of its submission that the report was normalized
  (Section 8) and answers `200` `{"acknowledged": true}` once. This is the single best-effort
  acknowledgment of ADR-0014 Section 1: nothing is bound or retained for it and it does not keep
  the Job resident. A repetition meets row 3 while the Job is resident and row 1 once it is
  retired. If the Job was claimed or retired before the lease, the refusals of Section 8 apply
  instead and nothing is recorded.

### 6. Answers

Every response with a status of `400` or above carries the error object of ADR-0008 Section 5,
with the same rules for `message` and `details`; the answer to a `HEAD` request carries the status
and headers of that response and no body. The conditions and codes of ADR-0008 Section 5 and
ADR-0009 Section 1 that concern the request itself (`domain` `request`) apply unchanged. The
conditions of this protocol are:

| Condition | Status | `domain` | `code` |
|---|---|---|---|
| The Job is not resident: unknown, never created, or retired (ADR-0014), found from the snapshot (Section 5, row 1), from a refused lease (Section 8), or by the reducer for a submitted `running` (Section 5) | `404` | `job` | `job_not_found` |
| `worker_id` is not the Worker bound to the Job, or the Job has no Worker yet (Section 5, row 2) | `409` | `worker` | `worker_mismatch` |
| The Job's process exit is confirmed (Section 5, row 3) | `409` | `worker` | `process_exited` |
| Any report while the Job's launch is not yet observed (Section 5, row 4) | `503` | `worker` | `launch_not_observed` |
| `completed` for a Job that is `preparing` with its launch observed (Section 5, row 8) | `409` | `worker` | `running_not_reported` |
| The writer has failed closed, including a commit whose outcome is unknown | `503` | `service` | `service_failed` |
| The writer refuses the report because admission is closed (`admission_closed`) | `503` | `service` | `not_ready` |
| The ingress queue is full (`normal_full`), which only `running` can meet, or a retirement has claimed the Job and is not yet committed (Section 8) | `503` | `service` | `busy` |
| A terminal report with the other `outcome` than the Job's acknowledgment binding records (Section 5, row 6), or one the writer answers `already_pending` | `409` | `worker` | `report_conflict` |
| The reducer rejects the report | `409` | `job` | the ADR-0002 rejection reason |
| Any other failure inside the daemon | `500` | `service` | `internal` |

- When several conditions hold, the first matching one in this order decides: the request
  conditions of ADR-0008 Section 5 in their order; then the rows of Section 5 in their order; then
  the answer of the writer (Section 8).
- A `503` means the same request may be sent again. After `launch_not_observed` and `busy` a
  repetition is expected to succeed; after `not_ready` and `service_failed` it does not succeed
  until the daemon is restarted or its unresolved Jobs are resolved. A `404` or a `409` is not a
  request to repeat: the same request gets another answer only after the Job's state has changed.
- An answer decided from the request or from the committed snapshot submits nothing to the writer,
  and a submission that the writer refuses or the reducer rejects is not appended (ADR-0002), so
  none of these creates a Journal record. `service_failed` is the exception: it includes a commit
  whose outcome is unknown (ADR-0006 Section 3), after which the record may or may not exist. A
  Worker must not infer from `service_failed` whether its report was recorded.

### 7. Late reports, reports after the exit, and retired Jobs

- v1 gives a Worker no way to make a late report on purpose. Before the exit is confirmed, a Job
  in `finalizing` or a terminal state holds the binding of its one terminal report, so another
  report is a repetition or a conflict (Section 5, rows 5 and 6). The only `late_worker_event` this
  adapter can cause is a terminal report that was past the snapshot check when the exit was
  confirmed; it is recorded and acknowledged once (Section 5).
- After the Job's process exit is confirmed, the binding is cleared (ADR-0002) and every report
  that names the Job's Worker is answered `409` `process_exited` from the snapshot (Section 5,
  row 3); a report with another `worker_id` is answered `409` `worker_mismatch` first (row 2).
- A report for a Job that the snapshot shows as not resident is answered `404` `job_not_found`
  (Section 5, row 1), and so is a terminal report whose lease is refused because the Job was
  retired after the snapshot was read (Section 8). A terminal report for such a Job reaches the
  writer in no form, as ADR-0014 Section 2 requires of a Worker fact for a retired Job. A `running`
  report that was submitted because the snapshot still showed the Job gets the writer's answer
  (Section 5): `404` only when it is admitted and the reducer rejects it, and otherwise the `503`
  of the refusal, for example `busy`.

### 8. What the adapter does toward the core

The Worker adapter is a producer in the sense of ADR-0003 and ADR-0012. It is one more client of
the core's internal API and adds no port (ADR-0005).

- It submits from the Worker listener's I/O thread only, one request at a time, through a
  lifetime-safe ingress registration of its own (ADR-0003). It waits for the completion of a
  submission after that submission has returned, never inside it, and takes it on that thread
  before it handles another request. It therefore holds at most one completion at any time.
- It submits a terminal report only through a Worker source lease of the Job that it acquired for
  that request (ADR-0003). A Job that is not resident grants no lease; that is the `job_not_found`
  of Section 6, decided with no ingress result and no failure latch (ADR-0014 Section 2), also
  when the Job was retired after the snapshot was read. A lease can be refused for other reasons,
  and the adapter answers each from the daemon's state, in this order: `service_failed` after a
  fail-closed latch; `not_ready` when the writer accepts no lease because admission is closed, as
  after a replay with unresolved Jobs or once shutdown has sealed the writer; `job_not_found` when
  the Job is not resident; and otherwise `busy`, which is the case of a Job that a retirement has
  claimed and not yet committed (ADR-0014 Section 1). The last two need a Job that became closed
  after its snapshot was read, since Section 5 answers a closed Job without a lease. A Job that
  replay restored has no Worker gate, and none is ever consulted for it: a restored Job with a
  bound Worker either has a binding or a confirmed exit, and is answered by Section 5, or is
  unresolved, and then admission is closed and the lease is refused as `not_ready`. The adapter
  returns the lease when the submission has been answered; a held lease and an admitted input each
  keep a Job from being retired (ADR-0014 Section 1), so no retirement can fall between the lease
  and the submission.
- It reads the committed snapshot for the rows of Section 5, before a submission and again after a
  terminal report's turn, and with the snapshot it reads whether the acknowledgment of the Job's
  binding is authorized (Section 4). The two are read together, so an answer never combines a
  binding with the authorization of another turn.
- The core's internal API gains two things. One is submitting a Worker fact through a lease the
  source acquired, with a completion that says whether the reducer recorded the fact as itself or
  normalized it to `late_worker_event`. The other is the read-only acknowledgment view just named:
  for a Job of this daemon's lifetime it turns true when the writer dispatches the ACK effect of
  the terminal-outcome turn, and for a Job that replay restored it is true from startup exactly
  when the restored snapshot is terminal and holds a binding (Section 4, step 5); it is false
  again once the exit clears the binding. Nothing waits on it. The implementation Issue names the
  exact operations. They change no rule of ADR-0003 or ADR-0014, and `sitometron_core` gains no
  public API.
- After a terminal report's turn succeeded, the adapter answers: `200` when the completion says the
  report was normalized to `late_worker_event`; otherwise by the rows of Section 5 from the
  snapshot and the view it reads then, which is `202` or `200` for the binding, or `409`
  `process_exited` when the exit was confirmed after the turn.
- It maps the writer's answers as Section 6 states: `admission_closed` to `not_ready`,
  `service_failed` to `service_failed`, `normal_full` to `busy`, and `already_pending` to
  `report_conflict`. `coalesced_pending` means the report was recorded by a turn the snapshot did
  not yet show; the adapter reads the snapshot again and answers by Section 5. `resident_limit`
  cannot occur for a report; if it does the answer is `500` `internal`.
- The composition sizes the core's callback registrations and completion storage for one more
  registration and one more completion, on top of every other producer.
- At shutdown the composition stops the Worker listener, which ends its I/O thread, before the
  writer seals, in the order ADR-0012 Section 5 gives producers. Closure of admission by the core
  does not stop the listener: until the composition stops it, it keeps answering, with the rows of
  Section 5 and with `503` `not_ready` for a report the writer refuses. After a fail-closed latch
  the listener likewise stays up and answers `503` `service_failed` for a submission; readiness is
  false from the latch, and the writer seals when the composition has stopped the listener, as for
  the runner (ADR-0016 Section 5).
- After a restart, replay reports a Job with a live or unknown Worker as unresolved and admission
  stays closed (ADR-0006). The rows of Section 5 answer its Worker's reports as for any Job, and a
  report that needs a submission is answered `503` `not_ready`. Resolving such Jobs is not decided
  here.

### 9. What a Worker must do

- Send `running` once it can do its work, and send it again on `503`. A Worker that never sends it
  never enters `running` (ADR-0016 Section 2).
- Send one terminal report when its work has ended, `completed` or `failed`, and repeat exactly
  that report until it is answered `200` `{"acknowledged": true}`, also across a restart of the
  daemon. Send `completed` only after a `running` was answered `204`; `failed` may be sent at any
  time. Treat `202` and `503` as "send again", and `404` and `409` as final. After `503`
  `service_failed` or `not_ready` it may give up, and it must not assume that its report was or
  was not recorded.
- Exit promptly after the acknowledgment, and exit when it gives up repeating. The daemon does not
  stop a Worker because it reported: until its exit is confirmed the Job stays resident (ADR-0014),
  and a Worker that does not exit is ended only by the timers of Issue #128.
- Choose its own pause between repetitions and its own limit. This contract fixes no duration.
- Treat a cooperative stop (ADR-0016 Section 3) as a request to end its work; it may still send its
  terminal report before it exits.

A Worker that exits without an acknowledged terminal report is decided by the facts the daemon has:
with no recorded report the reducer decides its exit by the Job's state, as for a process without
the protocol (ADR-0002; ADR-0016 Section 2), and with a recorded report the outcome is the one that
report led to.

### 10. Placement and machine-readable description

The routes are added to `sitometron_http` behind a Sitometron-owned Worker backend interface, next
to the existing Job backend interface, and the composition implements that interface on the core's
internal API. The adapter's server separates binding its address from accepting connections, which
Section 2 needs. The adapter adds no third-party dependency and the allowlists of ADR-0010 are
unchanged. The producer of `sitometron_process` takes the endpoint as one more value at
construction; its launch specification is unchanged. Worker HTTP v1 exists only in builds with HTTP
enabled, like the External REST adapter and the runner.

An OpenAPI document `schemas/openapi/worker-v1.openapi.json` describes Sections 3 and 6. By the
owner's decision of 2026-11-23 it is written after this ADR is Accepted, by the implementation
Issue, as ADR-0008 Section 8 did for External REST v1; until then the contract is the prose and
tables of this ADR, which remains the authority afterwards.

### 11. What changes in the Accepted ADRs

No sentence of an Accepted ADR is superseded.

- [ADR-0002](0002-define-core-job-reducer-contract.md): unchanged. Section 4 states how its
  acknowledgment sentence is met by a Worker-driven exchange answered from the committed binding.
  ADR-0002 says a late Worker event is "acknowledged without changing the result"; that describes
  an input that reaches the reducer, and the one report the exit can overtake is recorded and
  acknowledged so (Section 5). The adapter submits no report after the confirmed exit (Section 5,
  row 3); ADR-0002 does not require the Worker-protocol boundary to submit one.
- [ADR-0003](0003-define-single-state-writer-ingress-contract.md): unchanged. The Worker gate
  admits the one terminal report and refuses another while it is retained; its coalescing of a
  retransmission is not reached, because the adapter answers a repetition from the snapshot; the
  ACK effect stays the authorization point in a running daemon, which the acknowledgment view
  reports (Section 8); and v1 never asserts a successful ACK (Section 4). ADR-0003 says "An
  ADR-0002 ACK effect is itself ACK authorization at its declared effect-list position" and does
  not say what authorizes an acknowledgment after a restart, when no effect is dispatched; this
  ADR decides that case (Section 4, step 5) and changes nothing for a running daemon. ADR-0003's
  "a distinct post-terminal sequence may acquire the gate" stays true of the gate; this adapter
  does not submit one.
- [ADR-0005](0005-define-phase-0a-core-capability-port-contracts.md): unchanged. No port is added.
- [ADR-0006](0006-define-physical-jobjournal-durability-contract.md): unchanged. Replay still
  dispatches no effect and sends no acknowledgment. Answering a repetition that a Worker sends
  after the restart is not part of replay; the restored snapshot decides it (Section 4, step 5).
- [ADR-0008](0008-define-the-external-rest-v1-job-surface.md) and
  [ADR-0009](0009-settle-the-external-rest-v1-gaps-found-by-the-prototype.md): unchanged. Their
  transport, syntax, limit, and error rules are reused for a second listener.
- [ADR-0012](0012-drive-each-job-with-a-supervisor-in-the-core.md): unchanged.
- [ADR-0014](0014-retire-closed-jobs-under-capacity-pressure.md): unchanged. Its Section 1 speaks
  of a `late_worker_event` recorded after `process_exit_confirmed` and its single best-effort
  acknowledgment; under this ADR such a record arises only when the exit overtakes a report, and
  that acknowledgment is the one `200` of Section 5.
- [ADR-0015](0015-register-applications-with-a-verified-registry-file.md) Section 6: the question
  it assigned here is decided (Section 1): the External REST I/O model stays, and the Worker
  listener does not share its thread.
- [ADR-0016](0016-run-each-job-as-one-local-process.md): unchanged. Section 1's reserved namespace
  gains `SITOMETRON_WORKER_ENDPOINT`, and the question of Section 2 about a first report before the
  launch observation is decided (Section 5). Section 6 says the composition constructs the producer
  "before any listener opens"; this ADR binds the Worker listener's address before the producer is
  constructed and starts accepting on it afterwards, and reads "opens" as accepting connections, so
  that no connection is accepted before the runner exists.

### 12. Not decided here

- Worker bearer tokens and any authentication of a report (ADR-0004).
- Timer durations and the timer port (Issue #128), which end a Worker that never reports or never
  exits.
- A heartbeat, progress, results, logs, and Artifacts.
- Resolving Jobs that replay leaves unresolved (ADR-0006; #35, #73).
- A Worker protocol over any transport other than loopback HTTP.

## Requirements and checks

This ADR adds `WRK-002` (MUST): answer Worker HTTP v1 reports exactly as Sections 3 to 8 state, on
a loopback listener and I/O thread of its own when `--worker-listen` is given and on no listener
without it; give each process the endpoint of Section 2 when there is a listener; and never open a
connection to a Worker, hold a request open for an acknowledgment, or let a terminal report for a
Job that is not resident reach the writer. The sections are the rules; this sentence restates
none of their conditions.

Its named checks, Planned until the implementation Issue activates them. No check sleeps or reads a
host clock; orderings come from the adapter's submissions and the existing non-sleeping barriers:

- `worker_v1_listener_loopback_only`: a Worker listen address that is not loopback, a repeated or
  empty `--worker-listen`, a malformed one, and one with the address and nonzero port of `--listen`
  each refuse startup; both options with port 0 start and get different ports; without
  `--worker-listen` no Worker listener exists and the runner adds no endpoint variable; each
  listener answers the other's paths `404` `route_not_found`; no connection to the Worker listener
  is accepted before the runner is built;
- `worker_v1_endpoint_in_environment`: with a Worker listener, a started process has
  `SITOMETRON_WORKER_ENDPOINT` with the bound address and port, also for port 0, for an IPv6
  loopback address written in brackets, and when the daemon's environment carried another value;
- `worker_v1_running_after_launch_observed`: before the launch observation is committed, `running`,
  `completed`, and `failed` are each answered `503` `launch_not_observed` and submit nothing, for a
  Job that is `preparing` and for one that a cancel moved to `stopping` first, and the runner's
  observation is then decided by the reducer as without the reports; after it, `running` is
  answered `204` and the Job is `running`, and for a Job that is `stopping` with its launch
  observed a `completed` or a `failed` is submitted and accepted; a repeated `running` is answered
  `204` and adds no Journal record, also after
  admission has closed; a `completed` for a Job still `preparing` with its launch observed is
  answered `409` `running_not_reported` and submits nothing, and the same report is accepted once
  `running` was accepted; a `failed` for such a Job is accepted;
- `worker_v1_outcome_recorded_then_acknowledged`: a terminal report whose own turn has committed is
  answered `202` while the supervisor is held before `terminal_outcome_committed` (at a
  deterministic barrier); its repetition is still answered `202` while the writer is held after it
  installed the terminal snapshot and before it dispatched the ACK effect, and `200` once that
  effect is dispatched, for `completed` and for `failed`; no request stays open in between; a
  daemon stopped at that same point, after the terminal record is durable and before the ACK
  effect, answers the repetition `200` after its restart, and one stopped before the terminal
  record answers `202`; when
  the process exit is confirmed after the report's turn and before the adapter reads the snapshot
  again (held at a deterministic barrier), the answer is `409` `process_exited`;
- `worker_v1_repetition_answered_from_binding`: repetitions of a recorded report are answered
  from the committed binding, reach no ingress, and add no ingress sequence, reducer turn, or
  Journal record, before and after the terminal outcome is committed; a repetition is still
  answered `200` after an earlier `200` was written, and after the daemon is restarted with the
  Worker's exit unconfirmed; the recorded Worker event carries event sequence 1 for `completed`
  and 2 for `failed`;
- `worker_v1_conflicting_report_refused`: a terminal report with the other `outcome` is answered
  `409` `report_conflict` while a binding exists, in both directions, also after its
  acknowledgment and after a restart, and submits nothing;
- `worker_v1_identity_mismatch_refused`: a wrong `worker_id`, and a Job without a Worker, are
  answered `409` `worker_mismatch` and submit nothing;
- `worker_v1_report_after_exit_refused`: after the exit is confirmed every report that names the
  Job's Worker is answered `409` `process_exited` and one with another `worker_id` `409`
  `worker_mismatch`, and none adds a record, for a Job of this daemon's lifetime and for a resolved
  Job that replay restored; a terminal report that passed the snapshot check before the exit was
  confirmed (held at a deterministic barrier), and whose lease is granted, is recorded once as
  `late_worker_event`, changes no outcome, is answered `200` `{"acknowledged": true}` once, leaves
  no binding, and does not keep the Job from being retired; its repetition is answered `409`
  `process_exited` while the Job is resident and `404` `job_not_found` once it is retired;
- `worker_v1_job_not_resident`: a report for an unknown Job and for a retired Job is answered `404`
  `job_not_found`, with no ingress result, no Journal record, and no failure latch, also when the
  retirement happens between two reports; a terminal report whose snapshot was read before its Job
  became closed (held at a deterministic barrier) is answered `503` `busy` while a retirement has
  claimed the Job and not yet committed, and `404` once the retirement is committed; a
  `running` report whose Job is retired after the snapshot was read and before the submission
  (held at a deterministic barrier) is rejected by the reducer as `job_not_found` and answered
  `404` when it is admitted, is answered `503` `busy` when it meets `normal_full` instead, and in
  both cases adds no Journal record and no failure latch;
- `worker_v1_not_ready_and_busy`: a report the writer refuses with `admission_closed` is answered
  `503` `not_ready`, for a daemon started with unresolved Jobs and for one that is shutting down;
  in separate restart fixtures, an unresolved Job restored as `running` answers a repeated `running`
  `204` and a first terminal report `503` `not_ready`, and an unresolved Job restored as
  `finalizing` with a binding answers the repetition of its terminal report `202`;
  after a fail-closed latch the answer is `503` `service_failed`, also for a commit whose outcome
  is unknown and whose record exists at the next start; a `running` that meets `normal_full` is
  answered `503` `busy`; the listener keeps answering after admission closes and until the
  composition stops it;
- `worker_v1_listener_has_its_own_thread`: while an External REST handler is held outside any
  writer turn (at a deterministic barrier), the Worker listener answers a report decided from the
  snapshot and a terminal report that needs a writer turn; a report that needs a writer turn while
  a creation's turn is in progress waits for that turn, which is the shared writer and not the
  External REST thread;
- `worker_v1_request_rules`: the message-syntax, limit, media-type, strict-JSON, and field rules of
  ADR-0008 and ADR-0009 hold on the Worker listener, with the `outcome` values of Section 3; a
  request with an `event_sequence` field, or any other unknown field, is `422` `validation_failed`;
  a request target with a query component on a Worker route is `404` `route_not_found`
  (ADR-0009 Section 4); a `job_id` in the path that is not a canonical lowercase UUIDv7 is `400`
  `invalid_job_id`; a method other than `POST` on a route, `GET` and `OPTIONS` among them, is
  `405` `method_not_allowed` with `Allow: POST` (ADR-0009 Section 6); none of these submits
  anything;
- `worker_v1_error_envelope_and_codes`: every response of `400` or above on the Worker listener
  carries the error object of ADR-0008 Section 5 with the `domain` and `code` of Section 6, for one
  request error, each protocol condition, and each service condition, and no `message` or `details`
  contains a file-system path, a raw operating-system or library error, a Journal record, or a
  secret; a `HEAD` request on a route is answered `405` with `Allow: POST` and no body;
- `worker_v1_openapi_contract`: the OpenAPI document matches Sections 3 and 6;
- `rest_v1_worker_job_succeeds`: through production `sitometrond` with a verified registry and a
  Worker listener, a test Worker that reports `running` and `completed`, repeats until
  acknowledged, and exits brings its Job to `succeeded` with its exit confirmed and a recorded
  cleanup status, on Linux and Windows; a test Worker that reports `failed` brings it to `failed`.

## Consequences

- Good: a Job can succeed in production, with the three Worker events the reducer already has and
  no new port, event, or timer.
- Good: the daemon never connects to a Worker and never holds a request open, so no acknowledgment
  can block a thread, and a lost answer costs one repetition.
- Good: Worker reports have their own listener and thread, so they do not wait for the External
  REST thread, and External REST v1 is unchanged. They still share the one writer with every other
  input.
- Bad: a Worker must implement the repetition, and a Worker that stops repeating never learns that
  its report was acknowledged; its Job's outcome is unaffected.
- Bad: without credentials, any local process that knows a Job's identities can report for it.
- Bad: until Issue #128, a Worker that reports and does not exit keeps its Job resident.
- Bad: a report that arrives after the confirmed exit leaves no audit record.
- Good: the answer to a repetition comes from the committed binding and one read-only view of the
  core, so it is the same after a restart and needs no state in the adapter.
- Neutral: a second startup option and a second OpenAPI document.
- Bad: until the OpenAPI document exists, the contract is prose and tables only.

## Options considered

- **Hold the response to the terminal report until the acknowledgment is authorized**: rejected on
  2026-11-22 because it keeps a connection and the listener's thread for the length of the Job's
  finalization, needs a new wait in the core that also wakes on closure and failure, and still
  needs a rule for a lost response.
- **Let the daemon deliver the acknowledgment to a server in the Worker**: rejected on 2026-11-22
  because every Worker would need a listener and the daemon would need a retry interval and a
  timer of its own.
- **Write the OpenAPI document in the same pull request**: rejected on 2026-11-23, as it was for
  ADR-0008, to keep this decision small and to avoid fixing a machine-readable form of a contract
  that is still Proposed.
- **Let the Worker choose the event sequence**: rejected because the committed binding holds the
  Worker identity and the sequence but not the outcome, so a repetition with the other outcome
  under the same sequence could not be told from the recorded report after a restart; a sequence
  fixed by the outcome makes the binding identify the report.
- **End the obligation when the `200` is written**: rejected because a written response may not
  have arrived; the Worker's next repetition must get the same answer, and ADR-0002 already ends
  the binding at the confirmed exit.
- **Answer repetitions through the writer, by the gate's coalescing**: rejected because the gate is
  not restored by replay, so a Worker that outlives a restart could never be answered, and because
  the committed binding of ADR-0002 already holds the retry identity.
- **Take the acknowledgment from the terminal state of the snapshot alone**: rejected because the
  writer installs a turn's snapshot before it dispatches the turn's effects, and ADR-0003 makes the
  ACK effect the authorization point; a repetition could be acknowledged a moment too early.
- **Record a terminal report after the confirmed exit as a late event**: rejected on 2026-11-23
  because the Worker is gone by then, a Job that replay restored has no Worker gate to submit
  through, and each further report would need a rule of its own; a refusal from the snapshot needs
  none.
- **Submit a `failed` report before the launch observation**: rejected because the reducer would
  accept it and then reject the runner's observation, against the owner's decision that a first
  report waits for the observation.
- **Serve `/worker/v1` on the External REST listener**: rejected on 2026-11-22 because a report
  would wait behind a creation that holds the one I/O thread.
- **Replace synchronous completion waits with asynchronous ones on one listener**: rejected on
  2026-11-22 because it rewrites the External REST adapter and its checks for a problem a second
  thread solves.
- **Hold a `running` report in the adapter until the launch is observed**: rejected on 2026-11-22
  because it needs a new wait and holds the thread; the Worker's repetition needs neither.
- **Let the Worker wait a fixed time before its first report**: rejected because it depends on a
  duration and still fails when the observation is delayed longer.
- **Submit every `running` report and let the reducer reject it**: rejected because the reducer
  answers a too-early report and a repeated one with rejections that a Worker cannot tell from a
  real conflict.
- **A Worker bearer token in Phase 2**: rejected on 2026-11-22 because generation, lifetime,
  restart behavior, and secrecy rules are a design of their own (ADR-0004); loopback trust matches
  External REST v1 until then.
- **Stop a Worker once its terminal report is acknowledged**: rejected on 2026-11-22 because it
  adds a rule to the reducer and the supervisor table and stops a Worker that is finishing on its
  own.
- **A heartbeat**: rejected on 2026-11-22 because it needs a new event and a timer; the execution
  timeout and the runner's exit observation cover a Worker that hangs or dies.
- **One route for all reports with a kind field**: rejected because `running` and the terminal
  report have different bodies, answers, and input classes.
- **Carry the launch operation identity in a report**: rejected because the reducer binds Worker
  events by the Worker identity alone (ADR-0002), and a second identity would add a check with no
  contract behind it.

## References

- Issue #127; Gate #123 (P2-4); Issues #128, #126, #141, #73, #35
- Requirements: `WRK-002` (Planned)
- Contract Registry: `Worker HTTP v1 routes and JSON schemas`
- Related ADR: ADR-0002, ADR-0003, ADR-0004, ADR-0005, ADR-0006, ADR-0008, ADR-0009, ADR-0010,
  ADR-0012, ADR-0014, ADR-0015, ADR-0016
- `spike/README.md`: finding 9
