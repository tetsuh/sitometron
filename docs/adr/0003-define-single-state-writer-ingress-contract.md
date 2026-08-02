# ADR-0003: Define the single-state-writer ingress contract

## Status

Proposed — 2026-08-02

## Context

Accepted ADR-0002 defines a pure reducer, the closed Job lifecycle, and the logical Journal envelope.
It requires one state writer to decide an input, append and sync an accepted event before applying its
proposal or dispatching its effects, and leave rejected input without a Journal record. It does not
define how concurrently produced commands and lifecycle candidates reach that writer.

A normal bounded queue alone cannot preserve safety-critical work when normal traffic fills it. No
finite queue can preserve an unbounded producer population. An unbounded queue, a blocking producer,
a priority dequeue, or eviction of an admitted normal entry would instead make memory, producer
liveness, or accepted ordering unbounded or ambiguous.

The old candidate reserve `M * 4 + 3` counted only non-terminal Jobs. ADR-0002 permits terminal Jobs
to retain an ACK obligation, process-exit confirmation, resource release, cleanup, timer, or
quarantine producer. That candidate therefore cannot prove sufficient capacity.

This decision must preserve ADR-0002's closed reducer and audit semantics and ADR-0004's
dependency-minimal core boundary. It owns ingress admission and threading only. It does not define
Worker transport, HTTP status, process containment, physical Journal durability, or a production
shutdown deadline.

## Decision

We will use one strict, preallocated FIFO ingress and one state-writer execution context.

### Ownership and ordering

Exactly one writer owns mutable Job snapshots, reducer decisions, construction of the logical
Journal envelope and its durable `sequence`, pure apply, and ordered post-sync dispatch. An explicit
effect-executor port receives each typed effect synchronously in reducer-declared order. The executor
may schedule external work, but it owns no snapshot, cannot call apply, and reports later facts only
through ingress.

The ingress mutex owns only service mode, resident-slot reservations, FIFO/ring bookkeeping, critical
source gates and permits, callback registrations, and the non-persistent `ingress_sequence`. It is
never held while calling a Journal port, effect executor, response adapter, producer cancellation or
join, or external callback.

Three stages remain distinct:

1. **Ingress admission and linearization**: a distinct inserted input gets the next nonzero,
   non-wrapping `ingress_sequence`, initialized to 1 for each daemon process, while the ingress mutex
   is held. FIFO dequeue uses that order.
2. **Reducer acceptance**: the writer validates and decides the input against the current immutable
   snapshot. A reducer rejection is ADR-0002's stable rejection and creates no Journal event.
3. **Journal ordering**: only an accepted input receives an ADR-0002 logical envelope and the next
   nonzero, non-wrapping durable Journal `sequence`. Journal order is a distinct durable authority
   and is the order-preserving subsequence of successfully committed writer turns.

A coalesced retransmission and every not-admitted submission receive no new `ingress_sequence`,
reducer decision, or Journal record. Ingress-level results are not new ADR-0002 rejection reasons.
A coalesced result may identify the existing pending `ingress_sequence` for correlation only.

For each dequeued input, the writer performs exactly this order:

```text
validate and decide
  -> on accepted input, allocate durable sequence and construct logical envelope
  -> request logical commit
  -> only logical-commit success permits apply
  -> dispatch every reducer effect in its declared order
  -> release a command response
```

An ADR-0002 ACK effect is itself ACK authorization at its declared effect-list position; there is no
second ACK-authorization step after effect dispatch. A coalesced Worker retransmission authorizes no
ACK by itself. A reducer rejection, ingress non-admission, or logical-commit failure bypasses every
action forbidden by that result.

Durable-sequence exhaustion sets the failure latch before envelope construction. A sequence is
never wrapped, reused, or assigned to another event after a failed or unknown commit outcome. The
`job_logical_sequence_exhaustion_fail_closed` check covers the maximum-value boundary.
Phase 0A's fake Journal reports a logical commit result so orchestration can be tested. That evidence
does not claim filesystem flush, disk synchronization, crash durability, or any other Phase 0B
property.

### Resident Job bound

Startup configures a fixed, nonzero maximum resident Job population `J`. The ingress resident-slot
allocator is the sole authority for that bound. For a new Job ID, it atomically reserves a resident
slot and a normal FIFO slot before admitting `job_created`. Admission checks resident capacity first:
if both resident and normal capacity are unavailable, `resident_limit` takes deterministic
precedence. If only normal capacity is unavailable, the result is `normal_full`. On either failure no
slot is retained and no reducer input or Journal record is created.

A `job_created` for an already provisional or resident ID does not consume another resident slot.
Every admitted same-ID creation turn acquires a claim on that reservation before FIFO insertion. A
successful first creation converts the reservation into the permanent resident slot; later turns let
ADR-0002 decide `job_already_exists`. If an earlier turn is malformed or rejected, the reservation
remains while any same-ID creation turn or its completion callback is admitted or in flight. Only
when no creation claim remains, no creation succeeded, and every associated callback is impossible
may the provisional reservation be released. This covers malformed-first/valid-second and
valid-first/duplicate FIFO orderings.

Every successfully committed `job_created` holds its snapshot and resident slot for the Phase 0A
process lifetime. Producer quiescence retires source gates and permits but does not delete the
snapshot or permit slot reuse. A future Normative deletion/retention decision is required before
successful slots can be reused without changing ADR-0002's terminal-state rejection semantics.
Consequently at most `J` distinct Job IDs can be created successfully in one Phase 0A process. This
is a bounded Phase 0A limitation, not Phase 1 Admission or Phase 3 scheduling policy. Producer
registration and critical-permit acquisition require an existing resident or provisional slot.

The FIFO has these fixed, preallocated capacities:

```text
normal capacity       N > 0
critical reserve      R = 9 * J + 1
total capacity        T = N + R
```

All configuration arithmetic is checked before any producer is registered or any Job exists.
Multiplication and addition must not overflow, and every FIFO slot, resident slot, source gate, and
permit record needed by the proof must be preallocated. Failure is a startup failure. Runtime
resizing, allocation-on-enqueue, priority dequeue, normal-entry eviction, and blocking producer
enqueue are prohibited.

Normal admission requires both `normal_occupancy < N` and `total_occupancy < T`. Critical admission
requires a valid source permit and `total_occupancy < T`. Critical work may use an unused normal slot,
but normal work can never consume the final `R` slots.

### Closed input classes

These are the only critical FIFO inputs:

- administrator `terminate` command targeting a provisional or resident Job;
- `timeout_expired` notification from a registered phase-generation gate;
- `worker_completed` or `worker_failed` terminal/late Worker fact;
- `process_exit_confirmed`;
- `resources_released`;
- `cleanup_status_recorded`; and
- the global shutdown marker.

An administrator `terminate` command for an unknown Job ID uses normal admission so ADR-0002 can
return `job_not_found`. The `cancel` command and these raw candidates are also normal inputs:

```text
job_created
resources_committed
worker_launch_intent
worker_launch_observed
worker_running
session_retain_requested
session_retained
finalization_completed
finalization_failed
terminal_outcome_committed
```

Journal commit failure and readiness failure are service-control latches, not FIFO inputs, reducer
inputs, or Journal events. Adding or reclassifying an input requires a superseding ADR and concurrent
proof, requirement, Registry, and test updates.

### Critical source gates and proof

A critical source must acquire a gate and permit under the ingress mutex before it publishes,
schedules, or exposes the operation that can submit the identity. The permit remains held through
queued and in-flight writer processing and through any longer deduplication lifetime stated below.
A retransmission may coalesce only when its contract already has an authoritative delivery identity
and its entire semantic payload matches the identity retained at that gate. ADR-0002 supplies such
identity for timer generation and Worker event sequence; the shutdown operation has its daemon
generation. A command, process-exit observation, release fact, or cleanup fact has no separate
delivery identity and never coalesces merely because its payload is equal.

After a process-exit, release, or cleanup gate retires, a matching fact submitted later is a new
reducer input so ADR-0002's auditable idempotent duplicates remain Journaled. An exact Worker event
sequence is different: the Worker stop-and-wait gate retains at most one unacknowledged event
sequence and its exact semantic payload against its ACK obligation. An exact same-sequence
retransmission coalesces only while that ACK obligation is retained and receives no new ingress
sequence, reducer turn, Journal record, or ACK authorization. A pending or previously authorized ACK
may be retried by that protocol state; `coalesced_pending` itself never authorizes an ACK. After
successful ACK or confirmed process exit retires the Worker gate and identity, a distinct
post-terminal sequence may acquire the gate and reaches ADR-0002 as a Journaled and acknowledged
`late_worker_event`.

Every additional non-coalescible submission, different delivery identity, or different payload
presented while its gate is occupied receives `already_pending`. It is not an invariant violation and
remains owned by its source. Once a valid source registration and permit exist, ingress performs no
second lifecycle-state filter; ADR-0002 alone decides state validity, timeout generation validity,
late-event handling, and stable rejection.

| Source gate | Exact registration position and source owner | Pending identity and payload rule | Permit retirement | Bound | Proof check |
|---|---|---|---|---:|---|
| Administrator terminate | One preallocated gate for every provisional or resident Job in every lifecycle position from provisional `absent` through all terminal states; an unknown Job uses normal admission; command caller retains a non-admitted request | Gate key is Job ID plus terminate class; commands never coalesce because no retry identity exists; every additional command gets `already_pending` | Writer turn finishes or failure disposal releases the caller | 1 per Job | `job_ingress_capacity_and_reserve` |
| Active timeout | Timer owner acquires the phase gate before arming: preparation in `preparing`, execution from `running` through terminal commit, cooperative-stop in `stopping`, and process-exit-confirmation after terminal commit until exit confirmation; prior phase callbacks may remain queued or in flight across later positions | Job ID, one of the four ADR-0002 phases, generation, and exact notification payload; an exact same-generation delivery may coalesce; a different generation or payload at the occupied phase gate gets `already_pending` | Matching callback and writer turn finish, or disarm plus callback quiescence makes submission impossible; a replacement in the same phase waits for retirement | 4 per Job | `job_ingress_capacity_and_reserve` |
| Terminal/late Worker fact | After the launch binding exists but before the effect starts the producer, the gate is registered for the resident Job; it holds at most one unacknowledged Worker event sequence and its exact semantic payload across `preparing`, `running`, `stopping`, `finalizing`, terminal state, and confirmed process exit | Job ID, Worker ID, event sequence, kind, and exact payload; an exact same-sequence retransmission coalesces only while its ACK obligation is retained and receives no new ingress sequence, reducer turn, Journal record, or ACK authorization; a distinct sequence or payload while occupied gets `already_pending` and the source retains it | Successful ACK or confirmed process exit retires the Worker gate and identity; ACK success does not require process exit; only after retirement may a distinct sequence be admitted, and a distinct post-terminal sequence reaches ADR-0002 as a Journaled and acknowledged `late_worker_event` | 1 per Job | `job_ingress_coalescing` |
| Process exit | After the launch binding exists but before process start exposes a callback, the watcher gate is registered; it remains valid from launch observation through terminal state and watcher quiescence | Job ID, launch operation ID, and payload identify the fact but not a delivery retry; every additional observation gets `already_pending` | Writer turn finishes or watcher quiesces; a later matching audit uses a newly acquired gate | 1 per Job | `job_ingress_coalescing` |
| Resource release | After allocation commit but before a release operation can expose a callback, its gate is registered and may remain valid through every later lifecycle position | Job ID, allocation ID, digest, and payload identify the fact but not a delivery retry; every additional fact gets `already_pending` | Writer turn finishes or release source quiesces; a later matching audit uses a newly acquired gate | 1 per Job | `job_ingress_capacity_and_reserve` |
| Cleanup status | After terminal commit but before cleanup can expose a callback, its gate is registered and remains valid through later terminal cleanup positions | Job ID, cleanup status, and payload identify the fact but not a delivery retry; every additional fact or status gets `already_pending` | Writer turn finishes or cleanup source quiesces; a later matching audit uses a newly acquired gate | 1 per Job | `job_ingress_capacity_and_reserve` |
| Shutdown | One daemon-wide gate registered in `running`; shutdown caller owns the first marker | Daemon instance and shutdown generation; calls during `quiescing` attach while the marker is pending; calls from `draining` onward receive `admission_closed` | Retired atomically on entry to `sealed`, after every per-Job gate is retired, the FIFO is empty, and no turn is in flight | 1 global | `job_ingress_shutdown_quiescence` |

Queued and in-flight permits may survive the reducer transition that made another source eligible.
The reserve therefore does not rely on a current-state-only mutual-exclusion or cross-phase
callback-quiescence assumption. Its complete per-Job bound is the sum of independently enforceable
source-gate maxima:

```text
terminate              1
four timer phases       4
terminal/late Worker    1
process exit            1
resource release        1
cleanup status          1
                        -
per resident Job        9
```

This bound covers a retained preparation callback while execution and cooperative-stop timers are
armed, later process-exit-confirmation overlap, terminal cleanup before exit, and permits retained
across queued transitions. The four phase gates each allow only one generation at a time; reuse of a
phase gate waits for its prior callback to quiesce. The Worker stop-and-wait gate contributes one
per-Job bound: ACK success or confirmed process exit retires its identity, and ACK retry retains no
additional gate. No ACK window, deduplication collection, or additional reserve is added. At most `J`
resident Jobs therefore require `9 * J` permits, preserving the single per-Job Worker bound.
Shutdown requires one global permit, so `R = 9 * J + 1`. This is the smallest bound derivable from
the declared independent gates without adding an unaccepted cross-phase exclusion.

The obsolete `M * 4 + 3` is rejected because it excludes terminal residents, conflates Journal and
readiness failure with queue entries, and has no source-gate or permit-lifetime proof.

### Closed ingress results and source ownership

Ingress returns one of this closed transport-neutral result set:

```text
admitted(ingress_sequence)
coalesced_pending(existing_ingress_sequence)
already_pending
normal_full
resident_limit
admission_closed
service_failed
```

`coalesced_pending` only reports attachment to an existing delivery attempt. It is not reducer
acceptance, a response to the eventual command, or Worker ACK authorization.

| Submission and service condition | Result and ownership |
|---|---|
| New normal input in `running`, within `N` and `T` | `admitted`; ingress owns the queued input |
| New critical input with a valid free gate and capacity | `admitted`; ingress owns the queued input and permit |
| Exact retry with an authoritative delivery identity while its gate is pending, or repeat call for the same pending shutdown operation | `coalesced_pending`; original source/result relationship remains authoritative |
| Additional administrator command, or a different identity or payload at the same occupied source gate | `already_pending`; submitting source retains it |
| Terminate for an unknown Job | Use bounded normal admission; ADR-0002 later returns `job_not_found` |
| New Job with no resident slot, including simultaneous normal-capacity exhaustion | `resident_limit`; caller retains it; resident capacity is checked first |
| Normal input at `N` or total capacity, with any required resident slot available | `normal_full`; submitting source retains it |
| Any new admission in `quiescing`, `draining`, `sealed`, or `stopped` except registered critical completions allowed by the shutdown rule | `admission_closed`; source retains it |
| Any admission after the sticky failure latch, including after writer state reaches `stopped` | `service_failed`; source retains it; this result takes precedence over `admission_closed` |
| Critical admission cannot obtain its proof-required permit or FIFO slot under otherwise valid configuration | `service_failed`, atomically set the failure latch; source retains the obligation |

Malformed command/candidate payload validation remains writer/reducer input handling under ADR-0002;
it is not silently converted to queue pressure. A well-formed admitted candidate whose operation,
Session, allocation, Worker, or process-exit payload conflicts with the snapshot binding reaches the
ADR-0002 reducer, returns `invariant_violation`, and creates no Journal record or service-wide
failure. `service_failed` is reserved here for impossible internal corruption such as a registration
token referring to the wrong preallocated gate, a lease/permit ownership mismatch, or broken
accounting. That internal failure is not synthesized from an external candidate payload.

### Readiness and persistence failure

The service lifecycle modes are `running`, `quiescing`, `draining`, `sealed`, and `stopped`. A
monotonic `failure_latched` flag is orthogonal to those modes and remains observable after
`stopped`. Admission-mode and failure-latch changes linearize under the ingress mutex. A
readiness-failure callback immediately sets the latch, closes admission, and wakes the writer, but
it does not retroactively revoke the single writer turn already dequeued before that latch.

A writer turn is indivisible with respect to an asynchronous readiness latch:

- if no turn is in flight, no queued turn starts after the latch;
- a malformed or reducer-rejected turn releases its stable ADR-0002 rejection before the writer
  observes the latch;
- an in-flight shutdown marker completes its control transition before failure handling takes
  precedence;
- if an accepted turn's logical commit succeeds, the writer applies the committed proposal,
  dispatches all declared effects including any ACK effect, and releases its command response before
  observing the latch; and
- if that commit definitively fails or has unknown outcome, the persistence failure rule applies,
  the source receives `service_failed`, and no proposal, ordinary effect, success response, or ACK is
  released.

This rule prevents a committed Journal event from being left unapplied and gives every already
dequeued source one terminal disposition. The deterministic `job_ingress_readiness_failure` check
covers a latch before dequeue, during a rejection, while processing the shutdown marker, during
logical commit, after commit success, during apply, and during effect dispatch.

A logical commit failure is writer-local. It sets `failure_latched`, applies no proposal, dispatches
no ordinary reducer effect, and releases no success response or ACK. No later normal Journal sequence
is allocated in that process. Queued but unprocessed sources receive `service_failed` and retain
ownership; no queued input is treated as accepted or replayed.

After failure, the writer finishes the already-dequeued turn only under the rule above, releases
every other queued source without reducer decision, cancels producer registration, closes callback
admission, and derives only a finite safety-action list from the last committed snapshots. Such
actions may stop known work or quarantine resources but create no inferred Job transition and claim
no uncommitted outcome. After callback leases quiesce and no turn is in flight, the lifecycle moves
to `sealed` and then `stopped`; writer-owned snapshots can then be destroyed. A closed lifetime-safe
ingress endpoint may outlive them solely to return sticky `service_failed` to a late callback.

Detected reserve exhaustion under an otherwise valid proof, ingress- or durable-sequence exhaustion,
checked arithmetic failure, unexpected allocation failure, permit-accounting error, or foreign
exception sets `failure_latched` and follows the same failure path. It must not silently drop a
critical obligation, block a producer indefinitely, evict a normal entry, allocate an unbounded
fallback, or infer a transition.
Phase 0A bounds entry and action counts; it does not claim wall-clock bounds for production I/O.

### Shutdown and callback lifetime

The first shutdown request linearizes under the ingress mutex by closing normal admission and
inserting the one global shutdown marker. Normal entries already admitted before that marker retain
their FIFO order and drain before it. Repeated shutdown calls receive `coalesced_pending` only during
`quiescing` while the marker is pending. From `draining` onward they receive `admission_closed`,
unless the sticky failure latch requires `service_failed`.

When the writer reaches the marker, it enters `draining`, disallows new producer registration, and
cancels or detaches existing producers. Their already registered, bounded critical completions may
still enter. The writer drains those entries until every per-Job producer gate is retired, the FIFO is empty, and
no turn is in flight. It then retires the global shutdown gate atomically with entry to `sealed`.

Callbacks hold a lifetime-safe ingress handle rather than a raw writer or controller pointer. They
may perform only non-blocking validation and admission. They never call reducer apply, dispatch an
effect, or wait for their own entry. The handle refuses new access after closure and remains alive
until every admitted callback lease has returned. A detached producer receives only a closed handle
that has no path to writer-owned state.

Shutdown proceeds through these ordered conditions:

```text
running
  -> quiescing: close normal admission and insert/coalesce shutdown marker
  -> draining: process the marker; cancel or detach producers; permit bounded critical completions
  -> sealed: callbacks quiesced/joined, source gates retired, FIFO empty, no turn in flight
  -> stopped: writer-owned state destroyed; closed ingress endpoint may remain
```

No ingress mutex is held while invoking cancellation, callback, join, Journal, effect, response, or
ACK code. This is a count-bounded shutdown contract, not a production wall-clock shutdown guarantee.

### Dependency boundary for later implementation

Under ADR-0004 and `NFR-005`, synchronization is implemented privately in the existing
`sitometron_core` target. The later implementation may add only these four newly reviewed standard
headers to the reviewed core-header allowlist; they are private-implementation-only and are not
permitted in installed/public headers by this ADR:

```text
<condition_variable>
<memory>
<mutex>
<thread>
```

The exact dependency linkage is semantically `target_link_libraries(sitometron_core PRIVATE
Threads::Threads)`. Sitometron public APIs remain Sitometron-owned, expose no concurrency-owned or
dependency-owned types, and no third-party dependency is added. The later implementation must
extend the exact mechanical source/header/link/public-API checks accordingly, including the
approved-header, private-link-target, and public-API checks. If that exact list is insufficient or
conflicts with the accepted dependency boundary, implementation stops for a new decision rather
than expanding it implicitly.

## Verification model

The later implementation uses deterministic fakes, explicit turn barriers, and callback leases; it
uses no scheduler timing or real sleeps. In addition to the stable names registered in the build and
test specification:

- `job_ingress_capacity_and_reserve` fills normal capacity, retains all four timer-phase permits
  across lifecycle transitions, occupies the other five per-Job gates for every resident slot, then
  admits the global shutdown marker; it also covers malformed-first/valid-second and
  valid-first/duplicate provisional reservations, terminal terminate after producer quiescence, the
  `resident_limit` precedence when both limits are exhausted, and startup arithmetic boundaries;
- `job_ingress_coalescing` covers the Worker stop-and-wait gate holding at most one unacknowledged
  event sequence and exact payload: an exact same-sequence retry both while queued and after the
  writer turn coalesces only while its ACK obligation is retained, proving no new ingress sequence,
  reducer turn, Journal record, or ACK authorization; a distinct sequence or payload while occupied
  returns `already_pending` and remains with its source; successful ACK retires the gate without
  requiring process exit, and confirmed process exit also retires it; a distinct post-terminal
  sequence admitted after retirement reaches ADR-0002 as a Journaled and acknowledged
  `late_worker_event`; it separately proves that later process-exit, release, and cleanup duplicates
  are admitted and audited after their prior gates retire;
- `job_ingress_source_classification` covers unknown-Job terminate through normal admission,
  provisional/resident terminate through critical admission, no ingress lifecycle filtering after a
  valid permit, and ADR-0002 `invariant_violation` for a well-formed payload-binding conflict; and
- `job_ingress_fail_closed` distinguishes impossible internal registration/lease/gate/permit
  corruption from reducer rejection and proves bounded source disposition after the sticky failure
  latch.

## Consequences

- Good: A normal traffic burst cannot consume the critical capacity required by any resident Job.
- Good: Resident-slot and source-gate acquisition make the reserve assumptions enforceable.
- Good: Worker retries preserve protocol deduplication while later process-exit, release, and cleanup
  duplicates preserve ADR-0002 audit semantics.
- Good: FIFO ingress ordering, reducer acceptance, and durable Journal ordering cannot be conflated.
- Good: Readiness and commit-failure races cannot leave a committed event unapplied.
- Good: Callback lifetime and lock ownership are explicit before threading implementation begins.
- Bad: `J`, `N`, and a fixed preallocated capacity constrain compositions and require validation.
- Bad: Until a later Normative deletion/retention decision, Phase 0A can successfully create at most
  `J` distinct Job IDs during one process lifetime.
- Bad: Per-source gates and exact delivery identities increase bookkeeping.
- Bad: One already-dequeued normal turn may complete after readiness failure closes admission.
- Neutral: This ADR does not implement a queue, thread, production durability, Worker delivery, or
  public transport response.

## Options considered

- **Separate priority queues**: rejected because cross-queue ordering, starvation, and reservation
  accounting would introduce a second ordering policy beyond the strict FIFO.
- **Per-Job mailboxes**: rejected because global shutdown, cross-Job fairness, resident capacity, and
  terminal cleanup accounting would require an additional scheduler.
- **An unbounded queue**: rejected because finite capacity and shutdown obligations would remain
  unproven.
- **Blocking producer enqueue**: rejected because callbacks could deadlock while the writer or a
  shutdown path waits on the same producer.
- **Priority dequeue or normal-entry eviction**: rejected because either changes already admitted
  FIFO order or silently discards work.
- **Implicit/latest-wins coalescing**: rejected because it changes audit identity and could conceal a
  distinct ADR-0002 input.
- **Coalesce every matching ADR-0002 duplicate permanently**: rejected because matching process-exit,
  release, and cleanup duplicates remain auditable after a pending delivery finishes. Exact Worker
  event-sequence retransmissions remain the exception required by ADR-0002 protocol deduplication.
- **Assume at most two timer permits across phases**: rejected because ADR-0002 has four independent
  phase generations and callback quiescence may lag a later phase arm.
- **Reuse a successful terminal Job slot after producer quiescence**: rejected for Phase 0A because
  ADR-0002 has no deletion transition; forgetting the snapshot would change a terminal command
  rejection into `job_not_found`.
- **Queue Journal or readiness failure behind ordinary ingress**: rejected because a failing Journal
  cannot record its own failure and readiness loss must close admission without waiting behind normal
  traffic.
- **Retain `M * 4 + 3`**: rejected because it omits terminal residents and has no source-gate proof.

## References

- Issue #10
- Gate #1
- Requirements: `JOB-007`, `JRN-001` through `JRN-003`, `NFR-004`, `NFR-005`; proposed `JOB-008`,
  proposed `OPS-001`
- Contract Registry: proposed `Single-state-writer ingress and critical reserve`; affected `Core Job
  states and transitions`, `Core commands and rejection reasons`, `JobJournal envelope and event
  schemas`, and `Approved core dependency boundary and allowlist`
- Related ADRs: [ADR-0002](0002-define-core-job-reducer-contract.md),
  [ADR-0004](0004-allow-explicit-core-dependencies.md)
