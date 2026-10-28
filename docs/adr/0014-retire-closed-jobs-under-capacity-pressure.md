# ADR-0014: Retire closed Jobs under capacity pressure, oldest first

## Status

Proposed — 2026-10-28

## Context

ADR-0003 configures a fixed resident Job population `J` (`max_jobs`) and holds every successfully
created Job's snapshot and resident slot for the process lifetime: "A future Normative
deletion/retention decision is required before successful slots can be reused" (ADR-0003, "Resident
Job bound"). ADR-0006 Section 8 leaves "online pruning and in-memory resident retirement" to the
owner decision on finding H7 of Gate #50, which Issue #73 carried to Phase 1 and Gate #101 took as
P1-7. ADR-0008 Section 3 already says that a Job "that a later retirement decision removes" is
absent and answered like an unknown one, and its Consequences say resident retirement decides how
long a terminal Job stays readable. The walking skeleton's finding 4 (`spike/README.md`) observed
the consequence: `max_jobs` is the number of Jobs a daemon can ever accept, and the trace grows with
every turn.

The code fixes these facts:

- `JobOrchestrator::Impl::residents` is a vector of `max_jobs` slots; a slot is taken by a
  successful `job_created` and never freed (`src/core/job_orchestrator.cpp`, the `Resident` struct around line
  117 and the resident bound of ADR-0003). `SnapshotFor` finds a Job only through its resident
  (around line 2461), so a Job without a resident is absent to every reader.
- The global trace is one preallocated array of `trace_capacity` records that only grows; when it
  is full, `HasTraceCapacity` fails the next accepted turn (around lines 698 and 1407). A daemon's
  lifetime is therefore bounded by `trace_capacity` turns as well as by `max_jobs` creations.
- Replay seeds one resident per replayed Job (`Seed`, around line 170) and the Journal replay
  refuses to start when the fold reaches `max_jobs` Jobs (`src/journal/replay.cpp`, around line
  279, `journal_capacity_exceeded`, ADR-0006 Section 5). A retirement rule that replay cannot
  reproduce would make a daemon that created more than `J` Jobs impossible to restart.
- Each resident's timer gates carry a generation that the writer increments when a gate is reused
  (around lines 932 and 2007); the reducer contract validates a timeout against its phase generation
  before the candidate event, so a notification for an earlier generation is rejected as stale.
- ADR-0006 Section 6 defines an unresolved Job as non-terminal, with unreleased resources, or with
  no recorded cleanup status. A Job that is none of these is closed, which is also the condition
  ADR-0006 Section 8 uses for a prunable prefix.
- The Phase 1 composition keeps a creation-order index of `max_jobs` entries that it fills from
  replay and from successful creations (`apps/sitometrond/composition.cpp`, around line 107, Issue
  #106).

The owner decided on 2026-10-28 under Issue #103: a closed Job is retired only when a creation needs
its slot, and the oldest closed Job goes first, so that terminal Jobs stay readable while there is
room and the rule follows from Journal order alone.

## Decision

We will retire closed resident Jobs under capacity pressure, oldest first, with one rule that the
writer and replay apply identically, and we will make the trace a ring.

### 1. When a Job is retired

A resident Job is retirable when it is closed: its state is terminal, its resources are released,
and its cleanup status is recorded, and every per-Job source gate of ADR-0003 is retired and no
callback lease for it is in flight. A terminal Job whose cleanup status is `incomplete` while its
process exit is unconfirmed is closed too, because its cleanup status is recorded; ADR-0012 row 13
drives it no further, and this ADR retires it like any other closed Job.

Retirement happens only when a `job_created` is admitted and no resident slot is free. Then the
writer retires the retirable Job with the smallest `job_created` Journal sequence and gives its slot
to the new Job, in the same turn, before the new Job's `job_created` is applied. If no resident Job
is retirable, admission answers `resident_limit` as ADR-0003 says and nothing is retired. A closed
Job is never retired while a slot is free, and never because of time.

### 2. What retirement removes and what stays

Retirement resets the slot: the two state banks, the prepared effect and trace banks, the gate
registrations and identity slots, and the ADR-0012 post-commit mark for that Job. It increments
every timer-gate generation of the slot before reuse, so a notification armed for the retired Job is
rejected as stale by the existing generation rule. The ADR-0013 identity source keeps the retired
identifiers in its remembered set, as ADR-0013 Section 2 decided.

The Journal keeps every record (ADR-0006 Section 8); retirement is not a Journal event and appends
nothing. A later input for a retired Job reaches the reducer as an input for an absent Job and is
decided by the existing ADR-0002 rules, as ADR-0006 Section 8 already says of pruned Jobs.

### 3. External effects

Nothing in ADR-0008 changes. `GET /v1/jobs/{job_id}` for a retired Job answers `404` `job`/
`job_not_found`, as ADR-0008 Section 3 already states for a Job that a retirement decision removes.
The list holds the resident Jobs and no longer holds a retired one; it stays bounded by `max_jobs`
and ordered by `job_created` sequence. A cancel for a retired Job is a cancel for an unknown Job.
The Phase 1 creation-order index drops the retired identifier when the slot is reused.

### 4. Journal and replay

Replay applies the rule of Section 1 while it folds the Journal: when a replayed `job_created`
finds `max_jobs` Jobs in the fold, replay retires the replayed Job that is closed at that point in
the fold and has the smallest `job_created` sequence, and seeds the new Job in its place. Because
the rule depends only on the records and their order, the resident set after replay equals the
resident set the previous process held when it appended the last record. Replay refuses to start
with `journal_capacity_exceeded` only when a `job_created` finds `max_jobs` Jobs of which none is
closed at that point in the fold. The writer and replay share one implementation of the
retirability predicate and the oldest-first choice, so they cannot diverge.

Offline pruning (ADR-0006 Section 8) is unchanged: a pruned prefix removes closed Jobs from replay
entirely, which only reduces what replay has to retire. Because retirement keeps every record, a
Job retired in one run and still present in the retained Journal is retired again by replay, at the
same point.

This changes one sentence of ADR-0006 Section 5, the capacity refusal: "replay would create more
resident Jobs than the configured maximum" becomes "replay would create more resident Jobs than the
configured maximum after retiring closed Jobs under this ADR's rule". ADR-0006 is superseded in that
sentence only; this ADR names the superseding path, and every other rule of ADR-0006 stands.

### 5. The trace

The global trace becomes a ring of `trace_capacity` records. Ordinals keep increasing without bound;
when the ring is full, the oldest record is overwritten. A reader can rely on these properties: the
ordinals of the retained records are consecutive and end at the most recent one; a record is never
reordered or altered; and the per-turn prepared trace of one resident is still written as a whole,
in order, so a turn's records are either all retained or all overwritten. `HasTraceCapacity` no
longer fails a turn; it only sizes the per-turn prepared banks, which stay bounded as today. The
trace is diagnostic test support and no production reader depends on it.

### 6. The ADR-0003 change

ADR-0003 reserved this decision: "A future Normative deletion/retention decision is required before
successful slots can be reused without changing ADR-0002's terminal-state rejection semantics."
This ADR is that decision. It fills the reserved case and changes no other rule of ADR-0003: the
fixed population `J`, the resident-slot allocator as the sole authority for the bound, the
precedence of `resident_limit`, the provisional reservation rules, and the closed input classes are
unchanged. ADR-0002's terminal-state rejection semantics are unchanged because a retired Job is
absent, not terminal: its inputs are decided by the absent-entity rules that already exist for
pruned Jobs. No Journal event is added, so ADR-0002's event set and ADR-0003's input classes stay
closed.

### 7. Not decided here

- Online Journal pruning (ADR-0006 keeps it out of v1).
- A retention period or any time-based retirement; it would need the timer port (H11) and could
  not be derived from the Journal.
- Resolving Jobs that replay leaves unresolved (ADR-0006; #35, #73); an unresolved Job is not
  closed and is never retired.
- Exposing retired Jobs through any API (ADR-0008 Section 4 leaves that to a later ADR).

## Requirements and checks

This ADR adds `OPS-006` (MUST): reuse the resident slot of a closed Job only when a creation needs
it, retiring the closed Job with the smallest `job_created` sequence, with the writer and replay
applying one rule so that the resident set after a restart equals the set before it, and keep the
trace bounded without failing a turn.

Its named checks, Planned until the implementation Issue activates them:

- `job_retirement_oldest_closed_first`: with `max_jobs` residents of which several are closed, a
  creation retires the one with the smallest `job_created` sequence and succeeds; the retired Job is
  absent to `SnapshotFor` and the others are untouched;
- `job_retirement_requires_closed`: with `max_jobs` residents of which none is closed (terminal but
  unreleased, released but no cleanup record, or a gate still pending), a creation answers
  `resident_limit` and retires nothing;
- `job_retirement_not_under_free_slot`: with a free slot, a creation retires nothing, however many
  closed Jobs are resident;
- `job_retirement_resets_slot`: after retirement, the slot's banks, prepared banks, gate
  registrations, identity slots, and post-commit mark are those of the new Job, and every timer-gate
  generation is greater than before;
- `job_retirement_stale_timer_rejected`: a timeout notification armed for the retired Job's
  generation is rejected as stale after the slot is reused;
- `job_retirement_late_fact_absent`: a Worker or process-exit fact for the retired Job is decided by
  the absent-entity rules and appends nothing;
- `journal_replay_retires_like_writer`: a Journal written by a run that retired Jobs replays to the
  same resident set, with the same residents in the same order, as the run held at its last record;
- `journal_replay_capacity_after_retirement`: replay refuses with `journal_capacity_exceeded` only
  when a `job_created` finds `max_jobs` Jobs with none closed, and succeeds when one is closed;
- `job_trace_ring_wraps`: more turns than `trace_capacity` succeed, the retained ordinals are
  consecutive and end at the latest, and every retained turn is complete;
- `rest_v1_retired_job_absent`: after a retirement through the HTTP layer, `GET` of the retired Job
  is `404` `job_not_found`, the list omits it and keeps its order, and a cancel of it is `404`; and
- `rest_v1_daemon_outlives_max_jobs`: through the HTTP layer, more than `max_jobs` Jobs are created
  and closed in one process without a `capacity_exhausted` answer.

All checks drive the writer with the Issue #11 fakes and inject every input; none sleeps or reads a
host clock.

Traceability from Issue #103's required decisions:

| Required decision | Section | Checks |
|---|---|---|
| 1. When a Job is retired | 1 | `job_retirement_oldest_closed_first`, `job_retirement_requires_closed`, `job_retirement_not_under_free_slot` |
| 2. What retirement removes | 2 | `job_retirement_resets_slot`, `job_retirement_stale_timer_rejected`, `job_retirement_late_fact_absent` |
| 3. External effects | 3 | `rest_v1_retired_job_absent`, `rest_v1_daemon_outlives_max_jobs` |
| 4. Journal and replay | 4 | `journal_replay_retires_like_writer`, `journal_replay_capacity_after_retirement` |
| 5. The trace | 5 | `job_trace_ring_wraps` |
| 6. The ADR-0003 change | 6 | no check; the path is stated, and the ADR-0003 and ADR-0002 checks stay active unchanged |
| 7. Tests | this section | all of the above |

## Consequences

- Good: a daemon's lifetime is no longer bounded by `max_jobs` creations or `trace_capacity` turns;
  it serves Jobs as long as closed Jobs can be retired.
- Good: a terminal Job stays readable until its slot is needed, so a client that polls after
  completion sees the outcome in normal operation.
- Good: the rule is a pure function of Journal order, so replay reproduces the resident set and no
  Journal event or ADR-0002 change is needed.
- Bad: how long a terminal Job stays readable depends on load; under sustained creation a closed Job
  can become absent right after it closes. Clients read the outcome from the `202` or the first
  `GET`, as ADR-0009 Section 11 already advises.
- Bad: replay does more work per `job_created` when the fold is full, selecting the oldest closed
  Job; the cost is bounded by `max_jobs`.
- Neutral: one sentence of ADR-0006 Section 5 is superseded; the Journal format and the pruning
  rules are untouched.

## Options considered

- **Retire a Job as soon as it closes**: rejected because a completed Job would be absent to the
  first `GET` after its last cleanup record, which turns a routine client flow into a race, while
  capacity-pressure retirement gives the same lifetime guarantee.
- **Retire after a retention period**: rejected because it needs the timer port, which is not
  designed (H11), and because a time-based rule cannot be reproduced by replay from the Journal.
- **Record retirement as a Journal event**: rejected because it adds an event to ADR-0002's closed
  set and an input class to ADR-0003, two superseding ADRs for a fact the Journal already implies.
- **Let the source of ADR-0013 forget retired identifiers**: rejected on 2026-10-27 under Issue
  #102; the remembered set is kept for the source instance's lifetime.
- **Defer H7 to Phase 2**: rejected because the daemon would accept at most `max_jobs` creations per
  process, which is tolerable only while no Application is registered.
- **Keep the append-only trace and fail turns when it is full**: rejected because it bounds the
  daemon's lifetime by `trace_capacity` turns for a diagnostic structure no production reader uses.

## References

- Issue #103
- Gate #101 (P1-7); Gate #50 finding H7; Issue #73
- Requirements: `OPS-006` (Planned)
- Contract Registry: `Resident Job retirement and daemon lifetime`; `Single-state-writer ingress
  and critical reserve` (the reserved case is filled); `JobJournal startup validation, replay, and
  pruning` (one sentence superseded)
- Related ADR: ADR-0002, ADR-0003, ADR-0006, ADR-0008, ADR-0012, ADR-0013
- `spike/README.md`, finding 4
