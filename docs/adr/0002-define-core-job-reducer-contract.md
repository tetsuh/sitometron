# ADR-0002: Define the core Job reducer contract

## Status

Proposed — 2026-07-29

## Context

Sitometron requires one deterministic authority for each Job lifecycle decision. Earlier design notes
mixed Job states with facts such as `job_created`, `resources_committed`, and `worker_completed`.
They also left several races implicit, including cancel versus timeout, terminal Worker events versus
process exit, and successful computation versus failed finalization.

The Controller uses a single Job-state writer. A candidate input is decided against one immutable
snapshot. An accepted Journal event is appended and disk-synced before its state update or post-sync
effect is applied. A rejected input must not start I/O or produce a Journal event. The reducer must
therefore be pure, exhaustive, and independent of HTTP, Sitos, Worker wire formats, logging, and
platform APIs.

Phase 0A needs the logical Journal contract so that fake-driven reducer work can begin after this ADR
is accepted. Phase 0B separately owns physical serialization, Quill adoption, filesystem durability,
and the production JobJournal adapter.

## Decision

### Job states and snapshot

We will use this closed `JobState` enum:

```text
admitted
preparing
running
stopping
finalizing
succeeded
failed
cancelled
terminated
timed_out
```

The first five states are non-terminal. `finalizing` is a real non-terminal state, not a hidden flag.
The last five states are terminal. `absent` is an entity position used only to decide `job_created`;
it is not a Job state.

A reducer snapshot also carries closed internal fields needed by guards: durable Job, Session,
allocation, launch, and Worker bindings; latched reason; completion candidate and mode; resource and
Worker-launch status; process-exit confirmation; Session-retention status; finalization status;
cleanup status; and a pending terminal Worker-event acknowledgment. The strict snapshot schema is
[`job-reducer-snapshot.schema.json`](../../schemas/core/v1/job-reducer-snapshot.schema.json). Public
APIs may project these fields without introducing a second lifecycle state machine.

### Commands, candidate events, and Journal events

The command decision table owns the external Job controls `cancel` and `terminate`. An accepted
command produces `cancel_accepted` or `terminate_accepted`. A rejected command produces a stable
rejection reason and no Journal record.

Lifecycle subsystems submit strict typed candidate events without Journal sequence or timestamp.
The single state writer validates them, decides them, and supplies the accepted logical event with
the global sequence and diagnostic timestamp. The event decision table classifies every entity
position and event-kind pair as exactly one of:

```text
transition
audit
late_audit
reject
```

Accepted `transition` and `audit` cases append the named Journal event. `late_audit` normalizes a
delayed terminal Worker event to `late_worker_event`. `reject` appends nothing. After successful sync,
the reducer applies the declared updates and returns typed post-sync effects; it never executes an
effect itself.

We will use this closed core Journal event set:

```text
job_created
resources_committed
worker_launch_intent
worker_launch_observed
worker_running
cancel_accepted
terminate_accepted
timeout_expired
worker_completed
worker_failed
process_exit_confirmed
session_retain_requested
session_retained
finalization_completed
finalization_failed
terminal_outcome_committed
resources_released
cleanup_status_recorded
late_worker_event
```

The strict command, candidate-event, rejection, snapshot, and logical Journal DTOs are defined by the
JSON Schemas under [`schemas/core/v1`](../../schemas/core/v1/). The exhaustive cases use a closed
predicate AST and ordered-first-match semantics. Predicates, updates, and effects are normative in
[`job-reducer-contract.json`](../../schemas/core/v1/job-reducer-contract.json). No wildcard or
implicit default may hide a state/event pair, and every ordered case has a concrete reachable
fixture.

### Lifecycle and outcome rules

`job_created` creates an `admitted` Job and binds the Controller-issued Job and Session identity.
`resources_committed` enters `preparing` and carries the entire resolved allocation as a strict
core-neutral envelope containing its schema identity, version, UTF-8 JSON text bounded to 65,536
bytes, and SHA-256 digest over those exact bytes. Whitespace and key order are part of the identity;
ADR-0002 does not impose a second canonicalization algorithm on the Phase-owned payload schema.
Phase 3 owns the allocation payload schema and interpretation; ADR-0002 owns only the envelope and
the requirement to sync the exact resolved bytes before apply. `worker_launch_intent` binds its
stable operation ID, pre-generated Worker ID, verified Application identity, and committed allocation
identity before launch. `worker_running` must match the bound Worker ID before entering `running`.
Post-sync launch-effect arguments come from the synced intent payload; the v0.1 snapshot does not
serve as a replayable launch checkpoint.

Cancel or terminate accepted before a Worker process exists enters `finalizing` directly with
`completion_mode=process_already_exited`. Once a process may exist, cancel requests cooperative stop
and terminate requests forced stop through `stopping`. `stopping` is used only when an actual stop
action is required.

`worker_completed` in `running` records `completion_candidate=succeeded` and enters `finalizing`; it
does not commit success. `worker_failed` or unexpected `process_exit_confirmed` records `failed` and
enters `finalizing`. A terminal Worker event or confirmed process exit received in `stopping`
preserves the first latched reason and enters `finalizing`.

The first accepted cancel, terminate, execution timeout, Worker failure, or unexpected process exit
latches its applicable reason. Later terminate may request forced-stop escalation but cannot replace
that reason. A successful completion candidate is not a terminal reason and remains replaceable by a
finalization failure or execution timeout until terminal commit.

`finalization_completed` enables successful terminal commit. `finalization_failed` converts a sole
successful candidate to `failed`; an earlier cancel, terminate, timeout, or failure reason remains
unchanged. `terminal_outcome_committed` transitions from `finalizing` only when the payload outcome
matches the snapshot and a required finalization fact exists. Terminal state is applied only after
that event has been disk-synced.

### Timeout and post-terminal cleanup

The preparation timer covers synced resource commitment through synced `running`. The execution
timer covers synced `running` through synced terminal transition, including `finalizing`.
`finalizing` rejects new cooperative cancel but accepts expiration of the existing execution timer.
If only a successful completion candidate exists, that timeout latches `timed_out`. If another reason
is already latched, timeout or terminate escalation preserves it and requests only the necessary
force action.

`timeout_expired` has the closed phases `preparation`, `execution`, `cooperative_stop`, and
`process_exit_confirmation`, plus a non-wrapping timer generation. The single writer validates that
generation against its active timer before creating a candidate event; stale notifications never
become Journal events. Finalization operations also have bounded operation-specific deadlines.
A JobJournal append or sync failure is not a Journal event: it is a persistence-authority failure
that stops normal progress, latches readiness false, permits safety stop, quarantines resources, and
requires offline recovery and restart.

Normal success may be committed before process exit is confirmed. Terminal state and reason are
immutable, while completion mode and cleanup status remain separate audit axes. A post-terminal
`process_exit_confirmed` may set completion mode to `cooperative` or `forced`.
`timeout_expired{phase=process_exit_confirmation}` may request forced stop, quarantine, and readiness
failure without changing terminal state or reason.

### Rejections and late events

We will use this compact closed rejection catalogue:

```text
job_not_found
job_already_exists
command_not_allowed_in_state
event_not_allowed_in_state
stop_cause_already_latched
timeout_phase_mismatch
terminal_outcome_mismatch
required_finalization_fact_missing
invalid_event_payload
invariant_violation
```

Payload validation precedes matrix evaluation. U-12 will map these core reasons to the common typed
error catalogue and transport-specific responses; this ADR defines no HTTP status.

Worker delivery deduplication and sequence validation occur at the Worker-protocol boundary. A
Worker lifecycle event that arrives after an outcome or completion candidate is fixed is normalized
to `late_worker_event`, synced as an audit fact, and acknowledged without changing the result.
Operation, Session, allocation, Worker, and process-exit facts must match their snapshot bindings; a
conflicting identity is `invariant_violation` and is never applied. Process-exit facts carry the
launch operation ID so stale or misrouted process observations cannot terminate another launch.

Post-terminal process exit, resource release, and cleanup status are ordinary state-preserving audit
facts, not late Worker events. Matching duplicates are auditable and idempotent. A duplicate process
exit with a different completion mode, an allocation mismatch, or a cleanup-status regression is an
invariant violation.

### Journal authority boundary

ADR-0002 owns the logical minimum Job-event envelope:

```text
schema_version
sequence
event_type
recorded_at
job_id
payload
```

It also owns the closed core event kinds, payload discriminators used by the reducer, dispositions,
closed snapshot and predicate grammar, updates, and post-sync effect identifiers. Sequence is the
sole durable ordering authority; wall-clock time is diagnostic. Timer effects explicitly arm and
disarm preparation, execution, cooperative-stop, and process-exit-confirmation lifecycles. A bounded
finalization operation reports `finalization_failed` on definite failure or deadline instead of
inventing another Job timeout phase.

Phase 0B owns NDJSON encoding details, append/flush/disk-sync implementation, Quill integration,
filesystem behavior, fault injection, and the production Journal adapter. Accepting this ADR does not
claim that physical durability is implemented.

## Consequences

- Good: Every entity-position/event pair and every command/state pair has one machine-checkable
  disposition, concrete snapshot and payload fixture, expected Journal DTO, next snapshot, and
  post-sync effect list.
- Good: Journal sync remains the commit point for state changes, responses, and external effects.
- Good: First-cause races and successful-computation versus finalization-failure races are explicit.
- Good: Terminal outcome remains immutable while process cleanup can finish later.
- Bad: The reducer snapshot contains orthogonal internal fields in addition to the public state enum.
- Bad: The exhaustive matrix and vectors are larger than a handwritten switch statement.
- Neutral: Adapter-specific Sitos, Worker, HTTP, and filesystem payloads remain owned by later phases.
- Neutral: Implementation remains prohibited until this ADR is Accepted by the repository owner.

## Options considered

- **Reuse event names as states**: rejected because persisted facts and lifecycle positions have
  different stability and transition semantics.
- **Omit `finalizing` and use hidden flags**: rejected because completion, retention, terminal commit,
  and stop races would become an implicit second state machine.
- **Commit success at `worker_completed`**: rejected because manifest, retention, and terminal Journal
  commit can still fail.
- **Reject every timeout in `finalizing`**: rejected because external finalization I/O can stall and the
  existing execution deadline runs through terminal commit.
- **Allow post-terminal cleanup to rewrite the terminal result**: rejected because process cleanup is
  an independent axis and may occur after a valid computation result is durable.
- **Journal rejected commands**: rejected because rejection has no state effect or external commit and
  would make ordinary validation depend on persistence availability.
- **Use implicit default rejection rows**: rejected because missing pairs would silently change
  behavior when states or events are added.

## References

- [Issue #3](https://github.com/tetsuh/sitometron/issues/3)
- [Gate #1](https://github.com/tetsuh/sitometron/issues/1)
- Requirements: `JOB-001` through `JOB-007`, `JRN-001` through `JRN-003`
- Contract Registry: `Core Job states and transitions`; `Core commands and rejection reasons`;
  `JobJournal envelope and event schemas`
- Related ADR: [ADR-0001](0001-bootstrap-a-stdlib-only-cpp20-core.md)
