# ADR-0012: Drive each Job with a supervisor in the core

## Status

Proposed — 2026-10-14

## Context

ADR-0002 defines the Job states, events, and commands, and the pure reducer that accepts or rejects
each input. ADR-0003 defines the single writer, its ingress, its closed input classes, and the
critical source gates. ADR-0005 defines the capability ports the writer hands work to after a commit.
None of them names the component that submits the lifecycle events no other component produces:
`resources_committed`, `worker_launch_intent`, `session_retain_requested`, `finalization_completed`,
`finalization_failed`, `terminal_outcome_committed`, `resources_released`, and
`cleanup_status_recorded`.

No production code produces these events today. The core validates and applies them (for example
`src/core/job_orchestrator.cpp`, gate assignment and dispatch), and the walking skeleton's
`JobDriver` submits them by hand (`spike/README.md`, "What one Job does", and finding 1 of
"Findings for Phase 0B/1/2"). Gate #101 records the consequence for Phase 1: a production
`sitometrond` that admits a Job cannot carry it to a terminal outcome, so Phase 1 registers no
Application.

A cancel, a terminate, a timeout, or a Worker or process fact can arrive between any two of the
supervisor's submissions. External REST v1 finding 14 lists four landing points for a cancel. An
inventory of the normative reducer contract (`schemas/core/v1/job-reducer-contract.json`) for
Issue #104 finds 25 distinct cases across cancel, terminate, and timeout alone, before any Worker or
process fact is counted, including:

- a cancel before the launch intent still owes Session retention, finalization, the terminal
  outcome, and cleanup, not only the release; and
- an execution timeout after `finalization_completed` and before the terminal commit latches
  `timed_out` and clears the success candidate, so a prepared `terminal_outcome_committed{succeeded}`
  is then rejected with `terminal_outcome_mismatch`. ADR-0003 arms the execution timer "from
  `running` through terminal commit", so this is by design.

A list of cases written as prose would have to be complete to be correct, and every review could
find another case. The committed `Snapshot` (`include/sitometron/core/job_reducer.hpp`) already
carries every fact the next step depends on: the state, the latched reason, the success candidate,
the resource, launch, retention, finalization, and cleanup statuses, process presence, and whether
the process exit is confirmed.

ADR-0003 closes the ingress input classes; every event the supervisor submits is already in them.
Its source-gate table names the positions of the resource-release and cleanup-status gates but not
their owner. ADR-0005 adds no ingress, queue, or thread API for components outside the core.

The owner decided under Issue #104:

- on 2026-10-14, that the supervisor is specified as a function of committed state, with its cases
  in a machine-readable contract and a completeness check, not as a list of cases;
- on 2026-10-14, that the supervisor lives inside `sitometron_core` and submits through the
  existing ingress;
- on 2026-10-14, that the scope is the one stated under "Not decided here" below;
- on 2026-10-14, after the first review, that the decision function has no failure branches, and
  later phases add them inside the closed event set;
- on 2026-10-14, after the second review, that the overtake check derives its set from the reducer
  contract;
- on 2026-10-14, after the third review, that a Job refused with `normal_full` waits for a writer
  capacity signal; and
- on 2026-10-15, after the fourth review, that `JOB-009` holds while ADR-0003 admission is open, and
  a Job still open at shutdown or after the fail-closed latch is left to the next start's unresolved
  report.

## Decision

### 1. The supervisor

Each `sitometrond` has one supervisor, a private component of `sitometron_core`. It submits the
lifecycle events listed above and nothing else. It never applies an event, never decides whether an
event is accepted, and never performs I/O itself. It reads only committed state.

| Event | Producer |
|---|---|
| `job_created` | the External REST v1 create path, through `CreateJob` |
| `cancel_accepted`, `terminate_accepted` | the reducer, from the `cancel` and `terminate` commands |
| `resources_committed`, `worker_launch_intent`, `session_retain_requested`, `finalization_completed`, `finalization_failed`, `terminal_outcome_committed`, `resources_released`, `cleanup_status_recorded` | the supervisor |
| `worker_launch_observed`, `process_exit_confirmed` | the application runner (ADR-0005; Phase 2) |
| `worker_running`, `worker_completed`, `worker_failed` | the Worker protocol (Phase 2) |
| `late_worker_event` | the reducer, which normalizes a Worker fact that arrives after the Job left the states that accept it |
| `timeout_expired` | the timer port (H11) |
| `session_retained` | the Session retainer (ADR-0005; Phase 4) |

The supervisor is the source owner of the resource-release and cleanup-status critical gates of
ADR-0003, and registers them at the positions that table gives. This fills the owner that ADR-0003
leaves open; it adds no input class and changes no rule of ADR-0003.

### 2. The decision function

The supervisor decides every step with one pure function:

```text
Decide(snapshot, launch inputs) -> Submit(event, payload rule) | Wait(sources) | Done
```

`launch inputs` are the immutable inputs of a creation that the snapshot does not carry: the
requested Application. The supervisor keeps them in memory from creation until
`worker_launch_intent` commits.

The supervisor holds no step position. It calls `Decide` again, on a freshly read committed
snapshot, after each of these:

- the creation of the Job;
- the completion of any submission it made for the Job, admitted or rejected; and
- any commit for the Job by another producer. The writer signals this after the commit by marking
  the Job in a bounded set the supervisor drains; the set holds at most one mark per resident Job.

A mark is a coalesced notification: several commits the supervisor has not yet drained leave one
mark, and the supervisor decides once, on the latest committed snapshot. No skipped intermediate
snapshot needs a submission of its own. `Decide` depends on the snapshot alone, so an input that an
earlier snapshot assigned is either still assigned by the latest one or no longer owed.

The supervisor has at most one submission in flight per Job. A mark that arrives while a submission
is in flight is drained after that submission completes.

A rejection is never retried as the same input. The supervisor reads the snapshot again and decides
again. The late execution timeout above is handled this way: the second decision reads `timed_out`
and submits `terminal_outcome_committed{timed_out}`. If two consecutive decisions for one Job yield
the same input and both are rejected with the same code, the supervisor treats it as an invariant
breach: it stops driving that Job and reports it. The Job stays as committed. Whether such a breach
also latches the writer's failure state is not decided here.

The rules, first match wins:

| # | Committed state | Decision |
|---|---|---|
| 1 | no resident Job | `Done` |
| 2 | `admitted` | `Submit resources_committed` |
| 3 | `preparing`, launch `not_started`, launch inputs absent | `Done`; this happens only for a Job carried over a restart, whose resolution is not decided here |
| 4 | `preparing`, launch `not_started` | `Submit worker_launch_intent` |
| 5 | `preparing` after the intent, `running`, or `stopping` | `Wait`: application runner, Worker protocol, timer port, commands |
| 6 | `finalizing`, retention `not_started` | `Submit session_retain_requested` for the bound Session |
| 7 | `finalizing`, retention `requested` | `Wait`: Session retainer |
| 8 | `finalizing`, retention `retained`, finalization `pending` | `Submit finalization_completed` |
| 9 | `finalizing`, finalization `completed` or `failed` | `Submit terminal_outcome_committed` with the latched reason, or `succeeded` when no reason is latched and the success candidate is set |
| 10 | terminal, exit not confirmed, cleanup `pending` | `Wait`: application runner, timer port |
| 11 | terminal, exit confirmed, resources `committed` | `Submit resources_released` |
| 12 | terminal, cleanup `pending` | `Submit cleanup_status_recorded{completed}` |
| 13 | otherwise | `Done` |

The function decides from the committed snapshot and the launch inputs only. It has no failure
branch for Session retention, finalization, or resource release, because no component that can fail
at those steps exists yet: their placeholders (Section 6) never fail, and ADR-0005's retention
handoff returns no result. When a later phase gives one of these steps a way to fail, its design
decides how the failure reaches the supervisor and adds the rows. It must report the failure inside
the closed ADR-0002 event set: a retention or finalization failure through `finalization_failed`,
and a release failure through `cleanup_status_recorded{incomplete}`. A failure that needs an event
outside that set needs its own ADR (owner decision of 2026-10-14 under Issue #104).

Row 13 also covers a terminal Job whose process-exit-confirmation timeout has already recorded
cleanup `incomplete` while the exit is still unconfirmed. The supervisor does nothing more for it;
what happens to such a Job afterwards is not decided here.

### 3. The decision contract and its completeness

The rules above are normative. The implementation Issue writes them as a machine-readable decision
contract, `schemas/core/v1/supervisor-decision-contract.json`, in the pattern of
`job-reducer-contract.json`:
- each vector is a committed snapshot class, with the launch inputs present or absent, and the
  expected decision; and
- the 25 cases of the Issue #104 inventory are among its seeds.

`tools/validate_core_contract.py` gains two checks:

- every vector's expected decision follows from the rules; and
- every snapshot class that the reducer contract can reach from `job_created` has exactly one
  decision. Where the decision is `Submit`, the reducer contract accepts that event in that class.

### 4. Ingress results

The supervisor submits through the existing ingress of ADR-0003 and treats its results as follows:

- `admitted(sequence)` or `coalesced_pending(sequence)`: wait for the completion of that ingress
  sequence, then decide again.
- `normal_full`: park the Job and decide again once the writer frees normal capacity. After any turn
  that frees a normal slot, the writer sets one daemon-wide capacity signal. Like the post-commit
  mark, setting it is bounded, cannot fail, and does not re-enter ingress. The signal is
  level-triggered. The supervisor checks it after parking a Job, so a slot freed between the
  `normal_full` result and the parking is not lost. When it drains the signal, the supervisor
  decides again for every parked Job. Nothing busy-waits and no timer is involved.
- `already_pending`: cannot occur for the supervisor. Its normal inputs have no source gate. It is
  the sole owner of the resource-release and cleanup-status gates, and it never submits while its own
  submission is in flight. If the result does occur, the supervisor treats it as the invariant
  breach of Section 2.
- `resident_limit`: cannot occur for the supervisor, because it never creates a Job and every one of
  its submissions targets a resident Job.
- `admission_closed` or `service_failed`: stop driving. This happens during shutdown, while replay
  keeps admission closed, and after a fail-closed latch. Every open Job then stays as committed.

### 5. Threading

The supervisor runs on its own thread inside `sitometron_core`, which ADR-0003's private
synchronization headers already allow. It never runs on the writer thread and never inside a
handoff (ADR-0005). It holds no lock across an ingress call. The writer's post-commit mark (Section 2)
is a bounded, non-blocking update that cannot fail and does not re-enter ingress.

Shutdown stops the supervisor when ADR-0003 closes normal admission. After that point it makes no
new submission for any Job, including the critical release and cleanup inputs. It waits for the
completions of submissions that ingress already admitted, which drain in FIFO order before the
shutdown marker, and is joined with the other producers before ingress reaches `sealed`. It reads
no snapshot after that.

### 6. Placeholders until Phase 3 and Phase 4

- **Resources:** until the Phase 3 resource design exists, `resources_committed` carries one fixed
  empty allocation, schema `allocation.v1` with payload `{}` and its SHA-256 digest, as the walking
  skeleton does. Releasing it has no work and cannot fail.
- **Finalization:** until a later phase gives it work, finalization has none and cannot fail.
- **Session retention:** until the Phase 4 Sitos adapter exists, the composition's Session retainer
  placeholder reports `session_retained` for the same identity and never fails.

### 7. Not decided here

- Timer durations and the timer port (H11). The port submits `timeout_expired` with the phase and
  generation that ADR-0003's gate registered; nothing else about it is decided here.
- What a process without a Worker protocol reports, and the relation between `worker_completed` and
  `process_exit_confirmed` for it (Phase 2 application runner).
- Resolving Jobs that replay leaves unresolved, while ingress refuses every input for them
  (ADR-0006; #35, #73). The supervisor does not drive such a Job.
- What "if needed" means for the process-exit-confirmation timer (#107).
- Who executes the `set_readiness_false`, `quarantine_resources`, and `publish_terminal_result`
  effects (#108).

## Requirements and checks

This ADR adds `JOB-009` (MUST): while ADR-0003 admission is open, drive every Job that the running
daemon admits to a terminal outcome and a recorded cleanup status by submitting, after each
committed change, exactly the input the supervisor decision contract assigns to the latest committed
snapshot, and hold no other sequencing state. "After each committed change" means on the latest
committed snapshot after the change, with undrained commits coalesced as Section 2 states. Two kinds
of Job are outside it. A Job admitted before a restart: replay leaves it unresolved and closes
admission (ADR-0006), and Section 7 leaves its resolution open. A Job still open when shutdown
closes normal admission or the fail-closed latch is set: it stays as committed, and the next start
reports it as unresolved under ADR-0006, so its resolution is the same open question (#35, #73).

Its named checks, Planned until the implementation Issue activates them:

- `core_supervisor_decision_contract`: every vector matches the rules;
- `core_supervisor_decision_complete`: every reachable snapshot class has exactly one decision;
- `job_supervisor_drives_to_cleanup`: with fakes, a Job reaches each terminal outcome and recorded
  cleanup without a test-driven step;
- `job_supervisor_overtakes`: the overtake set is derived from the reducer contract, not listed.
  For each committed state whose decision is `Submit`, it takes every command and event that another
  producer of the Section 1 table submits and that the reducer contract does not reject in that
  state. This covers cancel, terminate, timeout, Worker, and process-exit inputs alike. Each one,
  injected between that decision and its submission, ends without a test-driven step in the outcome
  and cleanup the rules give;
- `job_supervisor_rejection_rederives`: a rejected submission is followed by a fresh decision, never
  by the same input;
- `job_supervisor_ingress_results`: a `normal_full` submission parks the Job and resumes after the
  writer frees a normal slot, including a slot freed before the parking, without polling; an
  `already_pending` result is reported as an invariant breach; an `admission_closed` or
  `service_failed` result, or the start of shutdown, stops all driving and leaves each open Job as
  committed; and two or more commits drained as
  one mark lead to one decision on the latest snapshot that still reaches the outcome and cleanup
  the rules give; and
- `job_supervisor_no_writer_reentry`: no supervisor code runs on the writer thread or inside a
  handoff.

## Consequences

- Good: one rule table decides every step, and completeness is a mechanical check over the
  reducer contract instead of a claim in prose.
- Good: the supervisor needs no public ingress API and no new input class.
- Good: Phase 2 can start implementation from an Accepted contract while Phase 1 is still being
  built.
- Bad: `sitometron_core` gains a thread and a bounded post-commit mark set.
- Bad: the launch inputs live only in memory until `worker_launch_intent` commits. A restart in that
  window leaves the Job unresolved, which is the same as for any open Job today.
- Bad: a Job still open when shutdown begins or the fail-closed latch is set is not driven further.
  The next start reports it as unresolved, as for a crash.
- Neutral: the placeholders of Section 6 stay until Phase 3 and Phase 4 replace them.

## Options considered

- **List every overtake case in this ADR**: rejected because completeness could only be claimed,
  not checked, and each review could add a case.
- **Reserve ingress capacity for supervisor inputs** so that `normal_full` cannot occur: rejected
  because it changes ADR-0003's closed input classes and capacity formula, which needs a superseding
  ADR.
- **Retry after `normal_full` on a timer**: rejected because the timer port is not designed (H11)
  and a timed retry makes the check depend on timing.
- **Treat `normal_full` as a breach**: rejected because a Job would be abandoned under load, which
  `JOB-009` forbids.
- **Drive or cancel open Jobs during shutdown**: rejected because `cancel` and the supervisor's
  normal inputs are refused once ADR-0003 closes normal admission. Admitting them would change
  ADR-0003's closed input classes or shutdown rule, which needs a superseding ADR, and finishing a
  Job during shutdown would launch Workers and retain Sessions, which is #35's process containment.
- **Place the supervisor in the composition root**: rejected because it needs the ingress,
  completion, and snapshot calls as a public port, which ADR-0005 excludes, so that ADR would need
  superseding.
- **Fold the supervisor's steps into the reducer**: rejected because the reducer is pure and decides
  one input at a time; submitting the next input is a separate role (ADR-0002, ADR-0003).
- **Add retention-failure and release-failure events**: rejected for now because
  `finalization_failed` and `cleanup_status_recorded{incomplete}` already carry both outcomes inside
  the closed event set.
- **Define a failure input and its delivery path now**: rejected because the components that could
  fail (Phase 3 resources, Phase 4 Sitos, later finalization work) are not designed yet, and the
  retention handoff of ADR-0005 returns no result.

## References

- Issue #104
- Requirements: `JOB-009`; relies on `JOB-001` to `JOB-008`, `OPS-004`
- Contract Registry: `Job lifecycle supervisor and decision contract`
- Related ADR: ADR-0002, ADR-0003, ADR-0005, ADR-0006
- `spike/README.md`: "What one Job does", External REST v1 finding 14, and findings 1, 5, 9, and 10
