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
the terminal report until the daemon's answer says it is acknowledged. When this ADR is Accepted,
Sections 1 to 9 become normative for every implementation; anything they do not state is not part
of v1. While it is Proposed it is no implementation authority.

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
  `/worker/v1` path and the Worker listener serves no `/v1` path; a request for the other's path
  that reaches route resolution is answered `404` `route_not_found`, after the refusals that
  ADR-0009 Section 1 decides before a route is known.
- Request and response bodies, the message-syntax rules, the limits, and the refusals before a
  route is known are those of External REST v1 (ADR-0008 Section 1, ADR-0009 Sections 1, 2, 4, 5,
  6, 7, and 8), applied to this listener with the same configured bounds. The decisions of the
  shared HTTP adapter apply to this listener unchanged as well: the bounds, time bounds,
  request-line handling, and `Expect: 100-continue` of ADR-0010 Sections 3 to 6, and the
  `malformed_json` rule, its known narrowing, and the parsing form of ADR-0011 Sections 2 to 4. The
  compatibility rules of ADR-0008 Section 2 apply to `/worker/v1` as they do to `/v1`.

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

A Worker learns the endpoint once, at its start, and has no way to learn another. A Worker that
outlives the daemon can therefore reach the restarted daemon only if it listens on the same
address and port again. An operator who wants that gives `--worker-listen` a fixed nonzero port
on every start; port 0 can bind another port after a restart, and a daemon restarted without the
option has no listener. Without the same endpoint the restarted daemon still holds the Job's
record, but its Worker cannot reach it and is not acknowledged.

The composition binds the Worker listener's address, so that its port is known, before it builds the
runner, and gives the endpoint to the runner's producer as a value of its own, next to the launch
specifications. Binding reserves the address and port only: the composition starts listening on the
socket, and accepting, after the runner is built and before the External REST listener opens
(Section 11). Until then a connection attempt to the endpoint does not complete.

### 3. Routes

Both routes take `POST` with a JSON object body. `{job_id}` is a canonical lowercase UUIDv7.
`worker_id` is a canonical lowercase UUIDv4. An unknown field, a missing field, a wrong type, and a
value out of range are `422` `validation_failed` (ADR-0008 Section 5); the strict JSON rules of
ADR-0009 Section 8 apply before that. A report without a body is `400` `malformed_json`, as
ADR-0009 Section 5 says of a creation, and whether a request has a body is decided by its framing,
as the shared adapter decides it for a creation. A request with no body framing, or with
`Content-Length: 0`, has no body: it has no media type to check and is `400` `malformed_json`
whatever its `Content-Type`. A request with chunked framing has a body even when the chunks carry
no data: the media-type check comes first, so it is `415` `unsupported_media_type` without a JSON
media type and `400` `malformed_json` with one.

| Route | Request body | Answers |
|---|---|---|
| `POST /worker/v1/jobs/{job_id}/running` | `{"worker_id": "..."}` | `204` with no body: the `running` report is accepted (Section 5) |
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
report. The daemon never opens a connection to a Worker. A request waits at most for the one
writer turn that records its own report (step 1); no request waits for the acknowledgment to be
authorized.

ADR-0002 keeps the acknowledgment obligation in the committed snapshot: a terminal Worker event
"binds an acknowledgment obligation to its Worker ID and event sequence", "Terminal commit does not
erase retry identity", and "the first accepted `process_exit_confirmed` clears the binding". The
adapter answers from that binding, so a report recorded in an earlier daemon lifetime is still
answered. Whether the answer is `202` or `200` depends on the authorization, which steps 2 and 5
define.

1. The Worker sends its terminal report. If no row of Section 5 answers it, which requires among
   other things that the committed snapshot holds no binding, the adapter submits the report to the
   writer as the matching critical input and waits for that turn (Section 8). Unless the confirmed
   exit overtook the report (Section 5), that turn records the report (`worker_completed` or
   `worker_failed`), moves the Job to `finalizing`, and binds the obligation; it does not authorize
   the acknowledgment.
2. ADR-0002 authorizes the acknowledgment in a later turn, the one that commits the matching
   `terminal_outcome_committed`. The supervisor drives the Job from `finalizing` to that record
   through its own inputs (ADR-0012), each in a writer turn of its own. ADR-0003 makes the ACK
   effect of that turn the authorization: "An ADR-0002 ACK effect is itself ACK authorization at
   its declared effect-list position". The writer installs a turn's snapshot before it dispatches
   the turn's effects, so a terminal snapshot alone is not yet the authorization. The adapter
   therefore asks the core whether the acknowledgment of the Job's binding is authorized
   (Section 8) and answers `202` `{"acknowledged": false}` while the binding names this report and
   it is not, and `200` `{"acknowledged": true}` once it is. While the binding still matches when
   the adapter reads the snapshot after the report's turn, the first answer is therefore `202`
   unless the authorization had already happened; Section 8 gives the answer when it no longer
   matches.
3. The Worker repeats the same report until it receives `200` `{"acknowledged": true}`, or a
   final answer, or until it gives up where Section 9 allows that; after a `202` it no longer may
   give up (Section 9). Section 5 alone decides what a repetition meets. While its row 5
   matches, which needs the binding in the current committed snapshot, the repetition is answered
   `202` or `200` by the same rule, without a submission. It therefore gets no ingress sequence,
   reducer turn, or Journal record, and it waits for nothing.
4. The daemon does not end the binding when it writes a `200`: a response that was written may
   not have arrived, and while row 5 still matches, the next repetition gets the same answer.
5. After a restart, replay "dispatches no post-sync effect" and "sends no acknowledgment"
   (ADR-0006 Section 6), so no ACK effect exists in the new daemon. By the owner's decision of
   2026-11-24, the durable `terminal_outcome_committed` record is then the authorization: for a
   Job that replay restored, the acknowledgment is authorized exactly when the restored snapshot is
   terminal and holds the binding. The ACK effect follows from that record without any further
   decision, so the record decides what the effect would have decided. This includes a daemon that
   stopped after the record was durable and before the effect was dispatched.

Steps 1 to 5 concern a report that establishes or meets a binding, and steps 2 and 5 are the whole
rule for when the acknowledgment of a binding is authorized. Which report meets the binding at all,
in a running daemon and after a restart, is decided by Section 5 alone. A submitted report that the
confirmed exit overtook establishes no binding and is outside these steps: Sections 5 and 8 decide
it, with the single acknowledgment that its own turn authorizes.

This is ADR-0002's "the Phase 2 adapter sends that idempotent ACK and retries until delivery or
confirmed process exit" for a protocol in which the Worker is the client: once the acknowledgment is
authorized the adapter sends it in every answer to a repetition of the report, for as long as the
Worker repeats; before that a repetition is answered `202`; and confirmed process exit ends the
obligation. Delivery is observed by the Worker, which stops repeating. ADR-0003, as ADR-0014 Section
6 words it, names three causes that retire the Worker gate: "Successful ACK, confirmed process exit,
or retirement under ADR-0014 retires the Worker gate and identity". v1 never asserts the first,
because the daemon cannot observe it; the other two apply as written. Because a repetition is
answered before ingress, ADR-0003's coalescing of an exact retransmission is not reached by this
adapter; the gate still admits the one terminal report and refuses any other while it is retained.

A terminal report with the other `outcome` than the binding's is a conflict (Section 5, row 6).

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

- The rows submit nothing and depend only on the snapshot and, for row 5, on the acknowledgment view
  read with it (Section 8), so they hold whether or not admission is open and whichever daemon
  lifetime recorded the Job: a repeated `running` or a repeated terminal report is still answered by
  its row after admission has closed and after a restart. A restart promises no earlier answer
  again: the rows are decided anew from the snapshot that replay folds from the durable records,
  which can hold a record the stopped daemon had made durable and not yet installed, such as a
  confirmed exit (row 3 instead of row 5), a terminal report whose commit outcome was unknown (row 5
  instead of `service_failed`), or a retirement (row 1). Whether a surviving binding is answered
  `202` or `200` is the authorization rule of Section 4.
- Row 3 is the owner's decision of 2026-11-23. It applies to every request whose snapshot check
  sees the confirmed exit, whichever process sent it and whenever it was sent: also to a report
  the Worker itself sent before it exited and that the adapter checked only afterwards. It is
  refused without a record.
- Row 4 covers every first report in every state in which the launch is not yet observed: a Job
  that is `preparing`, and a Job that a cancel, a terminate, or the preparation timeout moved to
  `stopping` before the runner's observation (ADR-0016 Section 2). The reducer would accept a
  `failed` there, and in `stopping` a `completed` too, and it would then reject the runner's
  observation, so the adapter submits none of them. The adapter holds no request until the launch
  is observed and arms no timer; the Worker's repetition is the only retry. Row 4 answers
  only while the snapshot shows the launch unobserved and the exit unconfirmed. Once either fact
  changes, the ordered table decides again: an observed launch lets the report pass row 4, and a
  confirmed exit, which an accepted failed launch observation gives without a separate exit fact
  (ADR-0016 Section 2), selects row 3.
- Row 8 guards `preparing` only, where the reducer rejects `completed`. In `stopping` the reducer
  accepts either outcome whether or not `running` was reported, and the report is submitted; that
  a Worker sends `completed` only after `running` is its obligation under Section 9, not a rule
  the adapter enforces there.

The snapshot can be overtaken before a submission is admitted. The writer and the reducer then
decide, the race itself latches nothing, and the adapter reads the snapshot again after a terminal
report's turn to answer by the rows above. A refusal, and a submission that was admitted and then
disposed or failed because the service failed, is answered as Section 8 maps it, in each of these
cases:

- a `running` whose Job was retired in between goes through normal admission as for an unknown Job
  (it is a normal input without a gate, ADR-0003): when its turn completes, the reducer has
  rejected it as `job_not_found` under ADR-0002's absent-entity rule, as ADR-0014 Section 2 says of
  a terminate for a retired Job, and the answer is `404`;
- a terminal report whose Job's exit was confirmed in between, and whose lease is still granted, is
  submitted. A refusal or a failure of the writer is answered as Section 8 maps it, as for any
  other submission: for example `503` `service_failed` when the service failed after the lease
  was acquired, or when the report's own commit failed or has an unknown outcome. When its turn
  succeeds, the reducer has normalized it to the `late_worker_event` of ADR-0002: it is recorded
  as an audit fact and changes no outcome, and ADR-0002 authorizes its acknowledgment with the
  record. The adapter learns from the successful completion of its submission, which follows the
  dispatch of that turn's effects, that the report was normalized (Section 8), and only then
  answers `200` `{"acknowledged": true}`, once. This is the single best-effort acknowledgment
  of ADR-0014 Section 1: the snapshot binds no retry obligation for it, the adapter makes no second
  attempt, and the record does not make the Job unclosed. The gate and the lease keep their rules:
  while the adapter holds the lease, or its admitted input is queued or in its turn, a retirement
  of the Job is refused (ADR-0014 Section 1). A permit that the Worker gate merely retains after
  that turn blocks nothing: the retirement proceeds and discards it, with no further
  acknowledgment (ADR-0014 Section 6). A repetition meets row 3 while the Job is resident and row 1
  once it is retired. If the Job was claimed or retired before the lease, the refusals of Section 8
  apply instead and nothing is recorded.

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
  the answer of the writer (Section 8). Within the validation stage of ADR-0008 Section 5 the body
  is validated before the `job_id` of the path, as the External REST adapter does: a report with
  an invalid field and a non-canonical `job_id` is `422` `validation_failed`.
- A `503` means the same request may be sent again. It promises no particular next answer: after
  `launch_not_observed` or `busy` a repetition can succeed, or meet another refusal, for example
  `process_exited` once the snapshot shows the exit confirmed or `job_not_found` when the Job is
  retired; after `not_ready` and `service_failed` it does not succeed until the daemon is restarted
  or its unresolved Jobs are resolved. A `404` or a `409` is not a request to repeat.
- An answer decided from the request, or from the committed snapshot before any submission,
  submits nothing to the writer, and a submission that the writer refuses or the reducer rejects
  is not appended (ADR-0002), so none of these creates a Journal record. There are two exceptions.
  `service_failed` includes a commit whose outcome is unknown (ADR-0006 Section 3), after which
  the record may or may not exist. And the `409` `process_exited` that Section 8 gives after a
  terminal report's own turn succeeded follows a report that is recorded: the exit was confirmed
  after that turn, and the Job's outcome is the one the recorded report led to. A
  Worker must not infer from `service_failed` whether its report was recorded.

### 7. Late reports, reports after the exit, and retired Jobs

This section decides nothing of its own; it says where each of these cases is decided.

- Late reports: v1 gives a Worker no way to make one on purpose, because before the exit is
  confirmed another terminal report is a repetition or a conflict (Section 5, rows 5 and 6). The
  only `late_worker_event` this adapter can cause is the report the exit overtook after its
  snapshot check (Section 5, the cases after the table).
- Reports after the confirmed exit: Section 5, rows 2 and 3.
- Reports for a Job that is not resident: Section 5, row 1, for a Job the snapshot does not show;
  Section 8 for a terminal report whose Job was retired after the snapshot was read, which reaches
  the writer in no form, as ADR-0014 Section 2 requires of a Worker fact for a retired Job; and
  Section 5, the cases after the table, for a `running` report submitted in that window.

### 8. What the adapter does toward the core

The Worker adapter is a producer in the sense of ADR-0003 and ADR-0012. It is one more client of
the core's internal API and adds no port (ADR-0005).

- It submits from the Worker listener's I/O thread only, one request at a time, through a
  lifetime-safe ingress registration of its own (ADR-0003). It waits for the completion of a
  submission after that submission has returned, never inside it, and takes it on that thread
  before it handles another request. It therefore holds at most one completion at any time.
- It submits a terminal report only through a Worker source lease of the Job that it acquired for
  that request (ADR-0003). A refused lease has exactly one reason, which the core decides at the
  lease attempt, in one observation and in this order, and returns with the refusal:
  `service_failed` after a fail-closed latch; `not_ready` when the writer accepts no lease because
  admission is closed, as after a replay with unresolved Jobs or once shutdown has sealed the
  writer; `job_not_found` when the Job is not resident, also when it was retired after the snapshot
  was read, decided with no ingress result and no failure latch (ADR-0014 Section 2); and otherwise
  `busy`, which is the case of a Job that a retirement has claimed and not yet committed (ADR-0014
  Section 1). The adapter answers with that reason and reads nothing again to find it, so a Job that
  was retired and a service that then failed is `service_failed`, and a claim that commits before
  the answer is written is still `busy`. The last two need a Job that became closed after its
  snapshot was read, since Section 5 answers a closed Job without a lease. A Job that replay
  restored has no Worker gate, and none is ever consulted for it: a restored Job with a bound Worker
  either has a binding or a confirmed exit, and is answered by Section 5, or is unresolved, and then
  admission is closed and the lease is refused as `not_ready`. The adapter returns the lease when
  the submission has been answered; a held lease and an admitted input each keep a Job from being
  retired (ADR-0014 Section 1), so no retirement can fall between the lease and the submission.
- It reads the committed snapshot for the rows of Section 5, before a submission and again after a
  terminal report's turn, and with the snapshot it reads whether the acknowledgment of the Job's
  binding is authorized (Section 4). The two are read together, so an answer never combines a
  binding with the authorization of another turn.
- The core's internal API gains two things. One is submitting a Worker fact through a lease the
  source acquired, with the reason of a refused lease as just described and a completion that says
  whether the reducer recorded the fact as itself or normalized it to `late_worker_event`. The other
  is the read-only acknowledgment view just named, which reports the authorization that Section 4,
  steps 2 and 5, define: it turns true at the ACK effect of the terminal-outcome turn or, for a Job
  that replay restored, is true from startup under step 5, and it is false again once the exit
  clears the binding. Nothing waits on it. The implementation Issue names the exact operations. They
  change no rule of ADR-0003 or ADR-0014, and `sitometron_core` gains no public API.
- A submission that is refused, or whose turn fails, is answered by the mapping below and never with
  `200` or `202`. After a terminal report's turn succeeded, the adapter answers: `200` when the
  completion says the report was normalized to `late_worker_event`; otherwise by the rows of Section
  5 from the snapshot and the view it reads then, which is `202` or `200` for the binding, or `409`
  `process_exited` when the exit was confirmed after the turn; in that last case the report stays
  recorded (Section 6). A fail-closed latch that comes after the turn committed never replaces this
  mapping with `service_failed`: a turn that was dequeued before the latch and committed
  (ADR-0003) is answered exactly as without the latch, with the `204`, the `202` or `200`, the
  single late `200`, or the `409` `process_exited` that the read after the turn gives.
- It maps the writer's answers as Section 6 states: `admission_closed` to `not_ready`,
  `service_failed` to `service_failed`, `normal_full` to `busy`, and `already_pending` to
  `report_conflict`. `coalesced_pending` cannot occur in v1: the adapter submits one request at a
  time and waits for its turn, so no report is queued when the next one is checked, and a report
  whose turn committed is answered by a row of Section 5 without a submission. `resident_limit`
  cannot occur for a report either. If one of them does occur the answer is `500` `internal`; the
  adapter never takes `coalesced_pending` as a sign that a report was recorded.
- The composition sizes the core's callback registrations and completion storage for one more
  registration and one more completion, on top of every other producer.
- At shutdown the composition stops the Worker listener, which ends its I/O thread, before the
  writer seals, in the order ADR-0012 Section 5 gives producers. Closure of admission by the core
  does not stop the listener: until the composition stops it, it keeps answering, with the rows of
  Section 5 and with `503` `not_ready` for a report the writer refuses. After a fail-closed latch
  the listener likewise stays up and answers `503` `service_failed` for a submission that is
  refused or whose completion reports a failure; readiness is
  false from the latch, and the writer seals when the composition has stopped the listener, as for
  the runner (ADR-0016 Section 5).
- After a restart, replay reports a Job with a live or unknown Worker as unresolved and admission
  stays closed (ADR-0006). The rows of Section 5 answer its Worker's reports as for any Job, and a
  report that needs a submission is answered `503` `not_ready`. Resolving such Jobs is not decided
  here.

### 9. What a Worker must do

- Send `running` once it can do its work. A Worker that never sends it never enters `running`
  (ADR-0016 Section 2).
- Send one terminal report when its work has ended, `completed` or `failed`, and never the other
  one afterwards. Send `completed` only after a `running` was answered `204`; `failed` may be sent
  at any time.
- Act on the result of each request as the table says. It covers every result, for both reports,
  where a result is a complete final response or the failure to receive one. An interim response,
  such as the `100 Continue` of ADR-0010 Section 6, is not a result: the Worker continues the same
  exchange, and an exchange that ends after only an interim response has no complete response.
  "The same report" has the identical `job_id`, `worker_id`, and, for a terminal report, `outcome`.

| Result of a request | What the Worker does |
|---|---|
| `204` to `running` | The report is accepted; it does not send `running` again |
| `200` `{"acknowledged": true}` to a terminal report | It stops repeating and exits promptly |
| `202` `{"acknowledged": false}` to a terminal report | The report is recorded and its acknowledgment is pending: it sends the same report again, for as long as it is answered `202`, whatever the number of repetitions, also across a restart of the daemon |
| `503` `launch_not_observed` | The daemon has not yet observed the launch: it sends the same report again and does not give up on this answer |
| Any other `503`, `500`, or `408` `request_timeout` | It sends the same report again, or gives up unless the repetition is mandatory (below) |
| No complete response: the connection fails, is closed, or ends before the response is complete | The result is unknown, and the report may or may not be recorded: it sends the same report again, or gives up unless the repetition is mandatory (below) |
| Any other response | Final: it stops sending that report. A final `404` or `409` does not prove that nothing was recorded (Section 6); any other `4xx` says that the request itself is wrong |

- Once a report has been answered `202` or `503` `launch_not_observed`, its repetition is
  mandatory: a later `503` of another code, `500`, `408` `request_timeout`, or exchange without a
  complete response does not let the Worker give up on it, and it sends the same report again.
  Only a `204` to `running`, a `200` to a terminal report, a final response, or a cooperative stop
  ends a mandatory repetition.
- A Worker that gives up on its terminal report, or whose terminal report got a final response,
  exits. One whose `running` got a final response or was given up cannot send `completed`; it sends
  `failed`, which is then its terminal report under the table, or exits. A
  Worker that gives up must not assume that its report was or was not recorded.
- Reporting does not by itself make the daemon stop a Worker, and until its exit is confirmed the
  Job stays resident (ADR-0014). The existing stops still apply to a Worker that has reported: a
  terminate while the Job is `finalizing` and its exit is unconfirmed (ADR-0002 rejects a terminate
  once the Job is terminal), and the forced stop of the runner's shutdown in every state (ADR-0016
  Section 5). Issue #128 adds the timers that end a Worker which does not exit when none of these
  happens, for a Job the running daemon drives. A Job that replay left unresolved is not driven
  (ADR-0012 Section 7) and replay arms no timer for it (ADR-0006 Section 6), so its restored binding
  can keep answering `202` for as long as the Job is unresolved, and its Worker keeps repeating
  until an operator stops it. Resolving such Jobs is not decided here.
- Choose its own pause between repetitions, and its own limit for the cases in which it may give
  up, which are those of a repetition that is not mandatory. This contract fixes no duration.
- Treat a cooperative stop (ADR-0016 Section 3) as a request to end its work; it may still send its
  terminal report before it exits. A cooperative stop also ends a repetition that the table makes
  mandatory: a Worker that is asked to stop may exit while its report is still answered `503`
  `launch_not_observed` or `202`.

A Worker that exits without an acknowledged terminal report is decided by the facts the daemon has:
with no recorded report the reducer decides its exit by the Job's state, as for a process without
the protocol (ADR-0002; ADR-0016 Section 2), and with a report recorded as `worker_completed` or
`worker_failed` the outcome is the one that report led to. A report recorded only as a
`late_worker_event` (Section 5) is an audit fact: its acknowledgment says that it was recorded,
and the Job's outcome is the one the exit decided.

### 10. Placement and machine-readable description

The routes are added to `sitometron_http` behind a Sitometron-owned Worker backend interface, next
to the existing Job backend interface, and the composition implements that interface on the core's
internal API. The adapter's server separates binding its address from listening and accepting on it,
which Section 2 needs. The adapter adds no third-party dependency and the allowlists of ADR-0010 are
unchanged. The producer of `sitometron_process` takes the endpoint as one more value at
construction; its launch specification is unchanged. Worker HTTP v1 exists only in builds with HTTP
enabled, like the External REST adapter and the runner.

An OpenAPI document `schemas/openapi/worker-v1.openapi.json` describes Sections 3 and 6. By the
owner's decision of 2026-11-23 it is written after this ADR is Accepted, by the implementation
Issue, as ADR-0008 Section 8 did for External REST v1; until then the contract is the prose and
tables of this ADR, which remains the authority afterwards.

### 11. What changes in the Accepted ADRs

When this ADR is Accepted, one sentence of an Accepted ADR is superseded: the ADR-0008 Section 1
sentence named under ADR-0008 below. No other sentence of an Accepted ADR is superseded, and until
acceptance that sentence stands as written.

- [ADR-0002](0002-define-core-job-reducer-contract.md): unchanged. Section 4 states how its
  acknowledgment sentence is met by a Worker-driven exchange answered from the committed binding.
  ADR-0002 says a late Worker event is "acknowledged without changing the result"; that describes
  an input that reaches the reducer, and the one report the exit can overtake is handled under the
  conditions of Sections 5 and 8. The adapter submits no report whose snapshot check sees the
  confirmed exit (Section 5, row 3), and Section 5 states the one exception, a report the exit
  overtook after that check; ADR-0002 does not require the Worker-protocol boundary to submit
  any other.
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
  [ADR-0009](0009-settle-the-external-rest-v1-gaps-found-by-the-prototype.md): their transport,
  syntax, limit, and error rules are unchanged and reused for a second listener. One sentence of
  ADR-0008's Decision is superseded at acceptance: "Paths outside `/v1` are not part of any
  contract." becomes "On the External REST listener, paths outside `/v1` are not part of any
  contract; the Worker listener serves the separate contract of ADR-0017, whose paths start with
  `/worker/v1`". On the External REST listener a `/worker/v1` path is still `route_not_found`
  (Section 1). This ADR names that superseding path, and every other rule of ADR-0008 stands.
- [ADR-0012](0012-drive-each-job-with-a-supervisor-in-the-core.md): unchanged.
- [ADR-0014](0014-retire-closed-jobs-under-capacity-pressure.md): unchanged. Its Section 1 speaks
  of a `late_worker_event` recorded after `process_exit_confirmed` and its single best-effort
  acknowledgment; under this ADR such a record can arise only when the exit overtakes a report,
  under the conditions of Sections 5 and 8, and that acknowledgment is the one `200` of Section 5.
- [ADR-0015](0015-register-applications-with-a-verified-registry-file.md) Section 6: the question
  it assigned here is decided (Section 1): the External REST I/O model stays, and the Worker
  listener does not share its thread.
- [ADR-0016](0016-run-each-job-as-one-local-process.md): unchanged. Section 1's reserved namespace
  gains `SITOMETRON_WORKER_ENDPOINT`, and the question of Section 2 about a first report before the
  launch observation is decided (Section 5). Section 6 says the composition constructs the producer
  "before any listener opens"; this ADR binds the Worker listener's address before the producer is
  constructed and starts listening and accepting on it afterwards. A bound socket that does not
  listen completes no connection, so the Worker listener opens after the producer exists and the
  sentence holds as written.

### 12. Not decided here

- Worker bearer tokens and any authentication of a report (ADR-0004).
- Timer durations and the timer port (Issue #128), which end a Worker that never reports or never
  exits, for a Job the running daemon drives.
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
  empty `--worker-listen`, one given as the last argument with no value, one followed by another
  option instead of a value, a malformed one, and one with the address and nonzero port of
  `--listen` each refuse startup; both options with port 0 on the same loopback address start and
  get different ports; without `--worker-listen` no Worker listener exists and the runner adds no
  endpoint variable; each listener answers a well-formed request for the other's paths `404`
  `route_not_found`, and a request refused before route resolution, such as one without a valid
  `Host`, keeps that refusal; while the construction of the runner's producer is held (at a
  deterministic barrier) a TCP connection attempt to the bound Worker endpoint establishes no
  connection, and after the barrier is released and the listener has started a new attempt succeeds;
- `worker_v1_endpoint_in_environment`: with a Worker listener, a started process has
  `SITOMETRON_WORKER_ENDPOINT` with the bound address and port, also for port 0, for an IPv6
  loopback address written in brackets, and when the daemon's environment carried another value;
- `worker_v1_running_after_launch_observed`: before the launch observation is committed, `running`,
  `completed`, and `failed` are each answered `503` `launch_not_observed` and submit nothing; in the
  fixture of a `preparing` Job the test Worker sends its `running` again after that answer, the
  runner's observation is then accepted, `running` is answered `204`, and the Job is `running`; in a
  separate fixture a cancel moves the Job to `stopping` before the observation, the reducer then
  rejects the observation, every report stays `503` `launch_not_observed` and reaches no ingress,
  and after the runner's exit report the answer is `409` `process_exited`; in a fixture whose launch
  fails and whose failed observation the reducer accepts, the answer is `409` `process_exited` with
  no exit report from the runner; in a third fixture the observation commits before the cancel, and
  for that `stopping` Job a `completed` or a `failed` is submitted and accepted; a `running` whose
  turn committed is answered `204` also when a cancel moves the Job to `stopping` before the answer
  is written; a `running` for a Job that is already `stopping` with its launch observed, and one for
  a Job that is `finalizing` with its exit unconfirmed, is submitted, rejected by the reducer, and
  answered `409` with the ADR-0002 rejection reason, never `204`; the same holds for a `running`
  against a terminal Job whose exit is unconfirmed, and none of these adds a record; a first
  `running` that is answered `408` `request_timeout`, for a delayed header and for a delayed body,
  submits nothing, and the same report sent again is answered `204`; a `running` whose turn
  committed and whose response is discarded before the Worker receives it is sent again by the same
  Worker, is answered `204`, reaches no ingress, and adds no second `worker_running` record; a test
  Worker that gives up on a `running` without a complete response sends `failed`, keeps repeating it
  through `202`, and exits at `200`; a repeated `running` is answered `204` and adds no Journal
  record, also after admission has closed; a `completed` for a Job still `preparing` with its launch
  observed is answered `409` `running_not_reported` and submits nothing, and the same report is
  accepted once `running` was accepted; a `failed` for such a Job is accepted;
- `worker_v1_outcome_recorded_then_acknowledged`: a terminal report whose own turn has committed is
  answered `202` while the supervisor is held before `terminal_outcome_committed` (at a
  deterministic barrier), where the test Worker keeps repeating through successive `202` answers and
  stops only at `200`; when the exchange after a `202` is interrupted, by a `408` `request_timeout`,
  by a `503` `busy`, and by a connection closed after only `100 Continue`, the test Worker sends the
  identical report again and is answered `200` once the ACK effect is dispatched; its repetition is
  still answered `202` while the writer is held after it installed the terminal snapshot and before
  it dispatched the ACK effect, and `200` once that effect is dispatched, for `completed` and for
  `failed`; no request stays open in between; a daemon stopped at that same point, after the
  terminal record is durable and before the ACK effect, answers the repetition `200` after its
  restart, and one stopped before the terminal record answers `202`; when the process exit is
  confirmed after the report's turn and before the adapter reads the snapshot again (held at a
  deterministic barrier), the answer is `409` `process_exited` and the Journal holds the report's
  record, also when another turn's commit fails and latches the service before that read;
- `worker_v1_repetition_answered_from_binding`: repetitions of a recorded report are answered from
  the committed binding, reach no ingress, and add no ingress sequence, reducer turn, or Journal
  record, before and after the terminal outcome is committed; a repetition is still answered `200`
  after an earlier `200` was written, and after the commit of another Job's turn failed and latched
  the service while the listener is still up, with no ingress, and after the daemon is restarted on
  the same fixed Worker address and port with the Worker's exit unconfirmed; in separate fixtures, a
  daemon restarted on another fixed Worker port, and one restarted without `--worker-listen`, hold
  the binding and are not reachable at the old endpoint; the recorded Worker event carries event
  sequence 1 for `completed` and 2 for `failed`;
- `worker_v1_conflicting_report_refused`: a terminal report with the other `outcome` is answered
  `409` `report_conflict` while a binding exists, in both directions, also after its
  acknowledgment and after a restart, and submits nothing;
- `worker_v1_identity_mismatch_refused`: a wrong `worker_id`, and a Job without a Worker, are
  answered `409` `worker_mismatch` and submit nothing;
- `worker_v1_report_after_exit_refused`: after the exit is confirmed every report that names the
  Job's Worker is answered `409` `process_exited` and one with another `worker_id` `409`
  `worker_mismatch`, and none adds a record, for a Job of this daemon's lifetime and for a resolved
  Job that replay restored, and for a report that the Job's own Worker sent before it exited and
  that is held before the snapshot check until the exit is confirmed (at a deterministic barrier); a
  terminal report that passed the snapshot check before the exit was confirmed (held at a
  deterministic barrier), and whose lease is granted, is recorded once as `late_worker_event`,
  changes no outcome, is answered `200` `{"acknowledged": true}` once, and leaves no binding in the
  snapshot; the same report is answered `503` `service_failed`, with no `200`, when the service
  fails after the lease is acquired and before the submission, when the report was admitted and is
  disposed because another turn's commit fails before it is dequeued, and when the report's own
  commit fails; a creation whose victim selection runs while the adapter still holds the lease (held
  at a deterministic barrier) is refused, and one whose victim selection runs after the lease is
  returned retires the Job and clears the permit the Worker gate still retains, without another
  acknowledgment; its repetition is answered `409` `process_exited` while the Job is resident and
  `404` `job_not_found` once it is retired;
- `worker_v1_job_not_resident`: a report for an unknown Job and for a retired Job is answered `404`
  `job_not_found`, with no ingress result, no Journal record, and no failure latch, also when the
  retirement happens between two reports; a terminal report whose snapshot was read before its Job
  became closed (held at a deterministic barrier) is answered `503` `busy` while a retirement has
  claimed the Job and not yet committed, and `404` once the retirement is committed; when the
  service fails after that retirement and before the lease attempt the answer is `503`
  `service_failed`, and when the claim commits after the lease was refused and before the answer is
  written it is still `503` `busy`; a `running` report whose Job is retired after the snapshot was
  read and before the submission (held at a deterministic barrier) is rejected by the reducer as
  `job_not_found` and answered `404` when its turn completes, is answered `503` `busy` when it
  meets `normal_full` instead, and is answered `503` `service_failed` when it was admitted and
  is disposed because another turn's commit fails first; the race itself adds no Journal record
  and no failure latch;
- `worker_v1_not_ready_and_busy`: a report the writer refuses with `admission_closed` is answered
  `503` `not_ready`, for a daemon started with unresolved Jobs and for one that is shutting down; in
  separate restart fixtures, an unresolved Job restored as `running` answers a repeated `running`
  `204` and a first terminal report `503` `not_ready`, and an unresolved Job restored as
  `finalizing` with a binding answers the repetition of its terminal report `202`, and keeps doing
  so, since nothing drives that Job; a daemon stopped after a `process_exit_confirmed` record was
  durable and before its snapshot was installed answered the repetition from the binding before the
  stop and answers it `409` `process_exited` after the restart; after a fail-closed latch a refused
  submission, and one whose completion reports a failure, is answered `503` `service_failed`, also
  for a commit whose outcome is unknown and whose record exists at the next start; a report whose
  turn was dequeued before the latch (held at a deterministic barrier) and then committed is
  answered by its completion, not `service_failed`; a `running` that meets `normal_full` is answered
  `503` `busy`; the listener keeps answering after admission closes and until the composition stops
  it;
- `worker_v1_listener_has_its_own_thread`: while an External REST handler is held outside any
  writer turn (at a deterministic barrier), the Worker listener answers a report decided from the
  snapshot and a terminal report that needs a writer turn; a report that needs a writer turn while
  a creation's turn is in progress waits for that turn, which is the shared writer and not the
  External REST thread;
- `worker_v1_request_rules`: the message-syntax, limit, media-type, strict-JSON, and field rules of
  ADR-0008 and ADR-0009 hold on the Worker listener, with the `outcome` values of Section 3; a
  request with an `event_sequence` field, or any other unknown field, is `422` `validation_failed`,
  and so is a `worker_id` that is missing, not a string, or not a canonical lowercase UUIDv4, on
  both routes, and an `outcome` that is missing, not a string, or neither `completed` nor `failed`;
  a request target with a query component on a Worker route is `404` `route_not_found` (ADR-0009
  Section 4); a `job_id` in the path that is not a canonical lowercase UUIDv7 is `400`
  `invalid_job_id`; a method other than `POST` on a route, `GET` and `OPTIONS` among them, is `405`
  `method_not_allowed` with `Allow: POST` (ADR-0009 Section 6); a report with both a `worker_id` of
  the wrong type and a non-canonical `job_id` is `422` `validation_failed` on both routes; none of
  these submits anything; the combined request bound, the time bounds, a request line with a higher
  `HTTP/1.x` version, and `Expect: 100-continue` behave as ADR-0010 Sections 3 to 6 state, and a
  leading byte-order mark, a lone surrogate, and a JSON parse error as ADR-0011 Sections 2 to 4
  state, with the same answers as on the External REST listener; on both routes, a request with no
  body framing and one with `Content-Length: 0` are `400` `malformed_json` with and without a JSON
  media type, and a chunked request without data is `415` `unsupported_media_type` without a JSON
  media type and `400` `malformed_json` with one; a body with a raw NUL, one with invalid UTF-8, one
  with trailing non-whitespace, and one with a duplicate key at any depth are each `400`
  `malformed_json` on a Worker route and reach no ingress (ADR-0009 Section 8);
- `worker_v1_error_envelope_and_codes`: every response of `400` or above on the Worker listener that
  has a body carries the error object of ADR-0008 Section 5 with its `domain` and `code`, for every
  request error that `worker_v1_request_rules` exercises (a malformed body, a failed validation, an
  invalid `job_id`, an unknown route, and a refused method among them), and for each protocol
  condition and each service condition of Section 6, and no `message` or `details` contains a
  file-system path, a raw operating-system or library error, a Journal record, or a secret; a `HEAD`
  request on a route is answered `405` with `Allow: POST` and no body;
- `worker_v1_openapi_contract`: the OpenAPI document matches Sections 3 and 6;
- `rest_v1_worker_job_succeeds`: through production `sitometrond` with a verified registry and a
  Worker listener, a test Worker that reports `running` and `completed`, repeats until acknowledged,
  and exits brings its Job to `succeeded` with its exit confirmed and a recorded cleanup status, on
  Linux and Windows; a test Worker that reports `failed` brings it to `failed`; a test Worker whose
  first terminal report is answered `408` `request_timeout`, once for a delayed header and once for
  a delayed body, sends the same report again and is acknowledged; and a test Worker whose terminal
  report is answered a transient `503` sends the identical report again and is acknowledged.

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
- Bad: until Issue #128, a Worker that reports and does not exit keeps its Job resident unless a
  terminate while the Job is still `finalizing`, or the daemon's shutdown, stops it. Once such a
  Job is terminal only the shutdown does.
- Bad: a report that arrives after the confirmed exit leaves no audit record.
- Good: the answer to a repetition comes from the committed binding and one read-only view of the
  core, so the adapter keeps no state, and after a restart every answer is decided anew from the
  replayed snapshot (Section 5).
- Neutral: a second startup option and a second OpenAPI document; one sentence of ADR-0008's
  Decision is superseded at acceptance (Section 11).
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
- Contract Registry: `Worker HTTP v1 routes and JSON schemas` stays `Planned | Planned` while this
  ADR is Proposed and becomes `Normative | Planned` when it is Accepted; the row of the
  `sitometrond` startup configuration and command line keeps `Planned | Implemented`, which covers
  the existing options only: `--worker-listen` is planned and not implemented; `External REST v1`
  keeps `Normative | Implemented` (one sentence of ADR-0008 superseded at acceptance, Section 11)
- Related ADR: ADR-0002, ADR-0003, ADR-0004, ADR-0005, ADR-0006, ADR-0008, ADR-0009, ADR-0010,
  ADR-0011, ADR-0012, ADR-0014, ADR-0015, ADR-0016
- `spike/README.md`: finding 9
