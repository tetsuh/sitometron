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
  successful `job_created` and never freed (`src/core/job_orchestrator.cpp`, the `Resident` struct
  around line 117 and the resident bound of ADR-0003). `SnapshotFor` finds a Job only through its
  resident (around line 2461), so a Job without a resident is absent to every reader.
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

A resident Job is closed when the Journal says so: its state is terminal, its `resources_released`
is recorded, and its `cleanup_status_recorded` is recorded. Only a closed Job is retirable, and the
predicate reads committed state alone; the gate and lease state of ADR-0003 never changes which Job
is retirable, so the writer and replay evaluate the same predicate over the same records. A
terminal Job whose cleanup status is `incomplete` while its process exit is unconfirmed is closed
too, because its cleanup status is recorded; ADR-0012 row 13 drives it no further, and this ADR
retires it like any other closed Job, retiring its process-exit gate with the claim below.

Retirement is decided where ADR-0003 places the resident bound: in the ingress resident-slot
allocator, under the ingress mutex, when a `job_created` for a new Job finds no free resident slot.
The allocator then claims the closed resident with the smallest `job_created` Journal sequence as
the victim and reserves its slot provisionally for the new Job, exactly as it reserves a free slot
today. The claim retires every gate of the victim that is still registered, so that no source can
acquire a lease for the victim afterwards. The allocator refuses rather than skips: if the victim
has a callback lease in flight at that moment, it does not choose the next closed Job but answers
`resident_limit`, and the caller retries the creation after the lease has finished. At most one
retirement claim is outstanding at a time; a creation that would need a second claim answers
`resident_limit` until the first creation has committed or released its claim. If no resident Job
is closed, admission answers `resident_limit` as ADR-0003 says and nothing is retired. A closed Job
is never retired while a slot is free, and never because of time.

The victim stays installed and readable through the writer's materialization and logical commit of
the new Job's `job_created`. Only after commit success does the no-fail snapshot installation of
ADR-0003 reset the victim's slot and install the new Job in it, in one step of the same turn. If
the creation is not committed (a reducer rejection, a precommit failure, or a logical-commit
failure), the claim is released as a provisional reservation is released today and the victim stays
resident; a failed commit latches the writer as ADR-0003 says and retires nothing.

### 2. What retirement removes and what stays

Retirement resets the slot: the two state banks, the prepared effect and trace banks, the gate
registrations and identity slots, and the ADR-0012 post-commit mark for that Job. It increments
every timer-gate generation of the slot before reuse, so a notification armed for the retired Job is
rejected as stale by the existing generation rule. The ADR-0013 identity source keeps the retired
identifiers in its remembered set, as ADR-0013 Section 2 decided.

The Journal keeps every record (ADR-0006 Section 8); retirement is not a Journal event and appends
nothing. A later input for a retired Job never reaches the writer through a critical source: the
claim retired the victim's gates, and ADR-0003 lets a critical source submit only through a
registered gate and a held lease, so a Worker fact, a process-exit observation, a release fact, or a
cleanup fact for a retired Job fails to acquire its lease and is dropped by its source, with no
ingress result, no reducer turn, no Journal record, and no failure latch. A terminate command for
the retired identifier uses normal admission, as ADR-0003 says of a terminate for an unknown Job,
and the reducer decides it by the existing ADR-0002 absent-entity rules, as ADR-0006 Section 8
already says of pruned Jobs.

### 3. External effects

Nothing in ADR-0008 changes. `GET /v1/jobs/{job_id}` for a retired Job answers `404` `job`/
`job_not_found`, as ADR-0008 Section 3 already states for a Job that a retirement decision removes.
The list holds the resident Jobs and no longer holds a retired one; it stays bounded by `max_jobs`
and ordered by `job_created` sequence. A cancel for a retired Job is a cancel for an unknown Job.
When the slot is reused, the Phase 1 creation-order index removes the retired identifier and
appends the new one, so the list keeps the surviving older Jobs before the new Job in ascending
`job_created` sequence.

### 4. Journal and replay

Replay applies the rule of Section 1 while it folds the Journal: when a replayed `job_created`
finds `max_jobs` Jobs in the fold, replay retires the replayed Job that is closed at that point in
the fold and has the smallest `job_created` sequence, and seeds the new Job in its place. Because
the rule depends only on the records and their order, the resident set after replay equals the
resident set the previous process held when it appended the last record. Replay refuses to start
with `journal_capacity_exceeded` only when a `job_created` finds `max_jobs` Jobs of which none is
closed at that point in the fold. The writer and replay share one implementation of the
retirability predicate and the oldest-first choice, so they cannot diverge; and because the writer
refuses a creation rather than skipping the oldest closed Job when runtime state blocks it (Section
1), every retirement the writer performed is the one replay performs at the same record.

Offline pruning (ADR-0006 Section 8) is unchanged: a pruned prefix removes closed Jobs from replay
entirely, which only reduces what replay has to retire. Because retirement keeps every record, a
Job retired in one run and still present in the retained Journal is retired again by replay, at the
same point.

This changes one sentence of ADR-0006 Section 5, the capacity refusal: "replay would create more
resident Jobs than the configured maximum" becomes "replay would create more resident Jobs than the
configured maximum after retiring closed Jobs under this ADR's rule". ADR-0006 is superseded in that
sentence only; this ADR names the superseding path, and every other rule of ADR-0006 stands.

### 5. The trace

The global trace becomes a ring of `trace_capacity` records. Ordinals keep increasing without bound.
A turn's prepared trace is appended to the ring as a whole: when fewer records are free than the
turn holds, the oldest retained turns are evicted whole, one after another, until the turn fits. A
turn never holds more than `trace_capacity` records because `HasTraceCapacity` still sizes the
per-turn prepared banks, which stay bounded as today, so whole-turn eviction always makes room. A
reader can rely on these properties: the ordinals of the retained records are consecutive and end
at the most recent one; a record is never reordered or altered; and every retained turn is complete,
because turns are retained and evicted only whole. `HasTraceCapacity` no longer fails a turn. The
trace is diagnostic test support and no production reader depends on it.

### 6. The ADR-0003 change

ADR-0003 reserved this decision: "A future Normative deletion/retention decision is required before
successful slots can be reused without changing ADR-0002's terminal-state rejection semantics."
This ADR is that decision. It fills the reserved case and changes ADR-0003 in these places, each
named here as its superseding path:

- "Resident Job bound": "For a new Job ID, it atomically reserves a resident slot and a normal FIFO
  slot before admitting `job_created`" becomes "it atomically reserves a free resident slot, or
  claims the slot of the oldest closed Job under ADR-0014 Section 1, and a normal FIFO slot before
  admitting `job_created`".
- The result-table row "New Job with no resident slot, including simultaneous normal-capacity
  exhaustion: `resident_limit`" applies when no slot is free and no closed Job can be claimed: none
  is closed, the oldest closed Job has a callback lease in flight, or a retirement claim is already
  outstanding.
- The "Permit retirement" column of every per-Job source gate gains one cause: the retirement claim
  of ADR-0014 Section 1 retires the gate.

Every other rule of ADR-0003 stands: the fixed population `J`, the resident-slot allocator as the
sole authority for the bound, the precedence of `resident_limit` over `normal_full`, the provisional
reservation rules, and the closed input classes. ADR-0002's terminal-state rejection semantics are
unchanged because a retired Job is absent, not terminal: its inputs are decided by the absent-entity
rules that already exist for pruned Jobs. No Journal event is added, so ADR-0002's event set and
ADR-0003's input classes stay closed.

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
  absent to `SnapshotFor` and the others are untouched; the victim is still readable until the new
  Job's commit, and with a logical-commit failure injected the victim stays resident and the writer
  latches;
- `job_retirement_requires_closed`: with `max_jobs` residents of which none is closed (terminal but
  unreleased, or released but no cleanup record), a creation answers `resident_limit` and retires
  nothing; with one closed Job whose callback lease is in flight, or while a retirement claim is
  outstanding, a creation answers `resident_limit` and retires nothing, and the creation after the
  lease or claim has finished retires that same Job;
- `job_retirement_not_under_free_slot`: with a free slot, a creation retires nothing, however many
  closed Jobs are resident;
- `job_retirement_resets_slot`: after retirement, the slot's banks, prepared banks, gate
  registrations, identity slots, and post-commit mark are those of the new Job, and every timer-gate
  generation is greater than before;
- `job_retirement_stale_timer_rejected`: a timeout notification armed for the retired Job's
  generation is rejected as stale after the slot is reused;
- `job_retirement_late_fact_absent`: a Worker or process-exit source fails to acquire its lease for
  the retired Job and submits nothing, without a failure latch; a terminate for the retired Job is
  decided as `job_not_found`; nothing is appended;
- `journal_replay_retires_like_writer`: a Journal written by a run that retired Jobs replays to the
  same resident set, with the same residents in the same order, as the run held at its last record;
- `journal_replay_capacity_after_retirement`: replay refuses with `journal_capacity_exceeded` only
  when a `job_created` finds `max_jobs` Jobs with none closed, and succeeds when one is closed;
- `job_trace_ring_wraps`: more turns than `trace_capacity` succeed, with turns of differing record
  counts; the retained ordinals are consecutive and end at the latest, and every retained turn is
  complete;
- `rest_v1_retired_job_absent`: after a retirement through the HTTP layer, `GET` of the retired Job
  is `404` `job_not_found`, the list omits it and lists the surviving older Jobs before the new Job
  in `job_created` order, and a cancel of it is `404`; and
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
- Bad: a creation can answer `resident_limit` while the oldest closed Job still has a callback lease
  in flight or another retirement claim is outstanding, although a closed Job exists; the next
  creation succeeds. The refusal keeps the writer's choice equal to replay's.
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
