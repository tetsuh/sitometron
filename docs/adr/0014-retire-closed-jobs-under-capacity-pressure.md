# ADR-0014: Retire closed Jobs under capacity pressure, oldest first

## Status

Accepted — 2026-11-01

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
- Each resident's timer gates carry a generation that the writer increments when a gate is re-armed:
  precommit materialization checks it against its maximum and computes the next value (around lines
  1092 to 1125), and the post-commit installation stores it (around line 1232); the reducer contract
  validates a timeout against its phase generation before the candidate event, so a notification for
  an earlier generation is rejected as stale.
- ADR-0006 Section 6 defines an unresolved Job as non-terminal, with unreleased resources, or with
  no recorded cleanup status. A Job that is none of these is resolved, which is also the condition
  ADR-0006 Section 8 uses for a prunable prefix; neither condition looks at the acknowledgment
  obligation of ADR-0002.
- The Phase 1 composition keeps a creation-order index of `max_jobs` entries that it fills from
  replay and from successful creations (`apps/sitometrond/composition.cpp`, around line 107, Issue
  #106).

The owner decided on 2026-10-28 under Issue #103: a closed Job is retired only when a creation needs
its slot, and the oldest closed Job goes first, so that terminal Jobs stay readable while there is
room and the rule follows from Journal order alone. On 2026-10-30 the owner decided that a Job whose
acknowledgment retry obligation under ADR-0002 is open is not closed, so retirement never cancels an
ACK retry and ADR-0002 is unchanged; on 2026-10-31 the owner decided that the single acknowledgment
of a `late_worker_event` recorded after `process_exit_confirmed`, which ADR-0002 does not retry,
does not keep a Job resident, and that retirement retires its Worker gate (Section 6 names the
ADR-0003 sentence).

## Decision

We will retire closed resident Jobs under capacity pressure, oldest first, with one rule that the
writer and replay apply identically, and we will make the trace a ring.

### 1. When a Job is retired

A resident Job is closed when the Journal says so: its state is terminal, it holds no committed
allocation (no `resources_committed` was recorded, or `resources_released` is recorded after it,
which is the resource condition ADR-0006 Section 6 uses for a resolved Job), its
`cleanup_status_recorded` is recorded, and, if any Worker event that binds an acknowledgment
(`worker_completed`, `worker_failed`, or the `late_worker_event` that ADR-0002 normalizes a delayed
Worker event to) was recorded for it, its process exit is confirmed in the snapshot the reducer
folds from the records: a `process_exit_confirmed` record, or a failed launch observation, which
ADR-0002 treats as the process having already exited. Replay folds the same records through the same
reducer, so this is a Journal-derived fact like the others. The last condition keeps ADR-0002's
acknowledgment retry rule intact: such an event binds an ACK obligation that the adapter retries
until delivery or the first accepted `process_exit_confirmed`, and ADR-0003 retains the Worker gate
for it until a successful ACK or the confirmed exit retires the gate, so a Job whose retry
obligation is open is not closed and is not retired. A successful ACK is not a Journal fact, so it
does not close the Job either: a Job whose terminal Worker event was acknowledged but whose exit is
not confirmed stays resident, and the writer never consults the gate's runtime state for the
predicate. A `late_worker_event` recorded after `process_exit_confirmed` is different: ADR-0002 ends
the retries at confirmed exit, so its acknowledgment is a single best-effort attempt with no retry
obligation; the condition is already met for it and it does not reopen the Job. The owner decided on
2026-10-31 that retiring such a Job also retires its Worker gate, dropping that single attempt if it
is still unacknowledged; this is not a cancellation of an ACK retry, and the ADR-0003 sentence it
changes is named in Section 6. Every condition is derived from the records alone, so the writer and
replay evaluate the same predicate over the same records; the gate and lease state of ADR-0003 never
changes which Job is closed. The closed set only grows. ADR-0002 accepts `resources_released` only
once the snapshot confirms the exit, and a Worker binding exists only after an allocation, so a Job
with an acknowledgment-binding Worker event closes only with its exit confirmed; a delayed Worker
event for a Job that never had a Worker binding is rejected as `invariant_violation` and records
nothing. No later record can make a closed Job unclosed. A terminal Job whose cleanup status is
`incomplete` while its process exit is unconfirmed after an acknowledgment-binding Worker event is
therefore not closed; ADR-0012 row 13 drives it no further, and it keeps its slot until its exit is
confirmed.

Admission. The ingress resident-slot allocator of ADR-0003 keeps one spare slot beyond `max_jobs`
for a creation that will retire a Job; `max_jobs` remains the bound on resident Jobs, and the spare
slot only ever holds a provisional creation. Under the ingress mutex, when a `job_created` for a new
Job finds no free resident slot, the allocator admits it with a retirement ticket if at least one
resident Job is closed in the committed state and no other ticket is outstanding; the ticket
reserves the spare slot provisionally, exactly as a free slot is reserved today, so the creation has
a provisional slot for its gates. The spare slot is a slot like the others for ADR-0003's gate and
reserve accounting: the allocator preallocates its gates and sizes the critical reserve for
`max_jobs + 1` slots, so a ticketed creation's terminate gate finds a permit even when every
resident permit and the shutdown permit are held (Section 6). Otherwise admission answers
`resident_limit` as ADR-0003 says and nothing is retired. Because the closed set only grows, a
closed Job still exists when the creation's turn comes. The ticket belongs to the spare slot's
provisional reservation, not to one turn: ADR-0003's same-identifier rules apply to that reservation
unchanged, so a later `job_created` for the ticketed identifier claims it without another ticket,
and whichever same-identifier turn is the first to proceed performs the check below. A refused or
rejected turn releases the ticket only when ADR-0003 would release the reservation: no creation
claim on the identifier remains and no associated callback is in flight. While the ticket is held, a
creation for another new identifier at full capacity answers `resident_limit`.

Selection. At the ticketed creation's turn, in the validate step of ADR-0003's turn order and
against the state after every earlier FIFO turn has been applied, the writer first asks whether
retirement is still needed: a ticket proves capacity pressure at admission, and an earlier
provisional reservation may have been released without a record since (a malformed or rejected
creation). If fewer than `max_jobs` Jobs are resident in the committed state, no retirement happens:
when an ordinary slot is free after counting the provisional reservations still held under
ADR-0003's same-identifier rules, the creation proceeds as an ordinary one into that slot and
releases the ticket; when every ordinary slot is either resident or still reserved, the turn ends
without a reducer decision, a Journal sequence, or a record, the creation's completion reports
`resident_limit`, the ticket is released under the rule above, and the caller retries once a
reservation has become resident or been released. Replay, which sees fewer than `max_jobs` Jobs at
that record and no record of any reservation, retires nothing either, so both branches keep the
writer and replay equal. Only when `max_jobs` Jobs are resident does the writer select the victim:
the closed resident with the smallest `job_created` Journal sequence, which is the one replay
selects at the same record (Section 4). Finding no closed resident at that point would contradict
the monotonicity above; the writer treats it as an invariant violation on ADR-0003's fail-closed
path, not as a capacity result. The writer refuses rather than skips: if any ownership of the
victim's slot is outstanding in ADR-0003's ingress bookkeeping, because a callback lease on it is in
flight, an admitted input for it holds a permit while queued behind the creation or in flight, or a
creation claim on its identifier is outstanding, the turn ends without a reducer decision, a Journal
sequence, or a record; the creation's completion reports `resident_limit`, the ticket is released
under the rule above, and the caller retries once that ownership has been released. No other closed
Job is chosen in its place, and no admitted entry is disposed of or reassigned. Otherwise the writer
marks the victim claimed under the ingress mutex. While the claim stands, no new ownership of the
victim's slot is granted: a source that asks for a lease is refused and keeps its fact, exactly as
for an unknown Job today; a terminate for the victim is admitted through normal admission, as for an
unknown Job, and the reducer decides it by the state it finds at its own turn (`job_not_found` after
the retirement, the terminal-state rejection if the creation was not committed); and a `job_created`
for the victim's identifier is admitted as for a new identifier, so it needs a free slot or a
ticket. The ingress mutex is not held across the commit; the claim is what keeps the bookkeeping
stable until commit or rollback.

Commit. The victim stays installed and readable through materialization and logical commit of the
new Job's `job_created`. Only after commit success does the no-fail snapshot installation of
ADR-0003 reset the victim's slot, retire its gates, and install the new Job in the spare slot, in
one step of the same turn; the victim's slot becomes the next spare. If the creation is not
committed, the claim is released, the ticket follows the reservation under the Admission rule above,
and the victim stays resident with its gates. After a reducer rejection the process goes on, and a
source refused during the claim may acquire its lease again. After a precommit or logical-commit
failure the writer latches as ADR-0003 says and retires nothing; the source keeps its fact, and no
lease is granted in the latched process because admission answers `service_failed` until a restart.
A closed Job is never retired while a slot is free, and never because of time.

### 2. What retirement removes and what stays

Retirement resets the victim's slot: the two state banks, the prepared effect and trace banks, the
gate registrations and identity slots, and the ADR-0012 post-commit mark for that Job; the reset
slot becomes the spare. It increments every timer-gate generation of that slot before any later
reuse, so a notification armed for the retired Job is rejected as stale by the existing generation
rule, and a timer callback that still holds no lease cannot acquire one, as Section 1 says. The
generations are non-wrapping (ADR-0002), so the increments are checked during the ticketed turn's
precommit materialization: if any generation of the victim's slot is at its maximum, the turn
fails before commit on ADR-0003's fail-closed path, the victim stays resident, and no `job_created`
is committed; the no-fail installation never meets an unincrementable generation. Retirement
removes nothing from the global trace: the records the retired Job's turns published stay in the
ring until ordinary whole-turn eviction (Section 5); only the slot's prepared trace banks are
reset. The
ADR-0013 identity source keeps the retired identifiers in its remembered set, as ADR-0013 Section 2
decided.

The Journal keeps every record (ADR-0006 Section 8); retirement is not a Journal event and appends
nothing. A later input for a retired Job never reaches the writer through a critical source:
retirement reset the victim's gates, and ADR-0003 lets a critical source submit only through a
registered gate and a held lease, so a Worker fact, a process-exit observation, a release fact, or a
cleanup fact for a retired Job fails to acquire its lease and is dropped by its source, with no
ingress result, no reducer turn, no Journal record, and no failure latch. A terminate command for
the retired identifier uses normal admission, as ADR-0003 says of a terminate for an unknown Job,
and the reducer decides it by the existing ADR-0002 absent-entity rules, as ADR-0006 Section 8
already says of pruned Jobs.

### 3. External effects

ADR-0008's answers are unchanged. `GET /v1/jobs/{job_id}` for a retired Job answers `404` `job`/
`job_not_found`, as ADR-0008 Section 3 already states for a Job that a retirement decision removes.
One sentence of ADR-0008's Decision is superseded: "`capacity_exhausted` does not clear without
operator action in this slice" becomes "`capacity_exhausted` clears when the oldest closed Job can
be retired: a resident Job is closed, no callback lease on the oldest closed Job is in flight, no
admitted input for it holds a permit queued or in flight, no creation claim on its identifier is
outstanding, and no other retirement ticket is outstanding, or when a ticketed creation that found
the committed set below `max_jobs` with the remaining slots provisionally reserved is retried after
a reservation became resident or was released"; a newer Job closing while the oldest closed Job is
still blocked does not clear it. A client may retry a create after that `503`. This ADR names that
superseding path, and every other rule of ADR-0008 stands. The list holds the resident Jobs and no
longer holds a retired one; it stays bounded by `max_jobs` and ordered by `job_created` sequence. A
cancel for a retired Job is a cancel for an unknown Job. When a Job is retired, the Phase 1
creation-order index removes the retired identifier and appends the new one, so the list keeps the
surviving older Jobs before the new Job in ascending `job_created` sequence. A creation whose
completion reports `resident_limit` (Section 1) is answered as the ingress result of the same name
is answered today: `503` `service`/`capacity_exhausted`, before any `202`, as ADR-0008 Section 3
orders the writer's results.

### 4. Journal and replay

Replay applies the rule of Section 1 while it folds the Journal: when a replayed `job_created` finds
`max_jobs` Jobs in the fold, replay retires the replayed Job that is closed at that point in the
fold and has the smallest `job_created` sequence, and seeds the new Job in its place. Because the
rule depends only on the records and their order, the resident set after replay is a function of the
durable Journal alone, and the durable Journal is the restart authority (ADR-0006 Section 3): the
guarantee is stated against that authority alone. The resident set after replay of the retained
Journal equals the set the previous process held after its last fully installed turn (a turn whose
`Commit()` returned `kCommitted` and whose no-fail snapshot installation completed), less any Job
that offline pruning under ADR-0006 Section 8 removed from the retained Journal in between, which is
absent after replay. The equality is stated for a replay under the same configured `max_jobs`: the
fold of one Journal with a larger `max_jobs` keeps Jobs the previous process retired, and with a
smaller one retires more, refuses with `journal_capacity_exceeded`, or refuses with
`journal_corrupt` under ADR-0006's unchanged validation when a retained record (a post-closure audit
such as a duplicate cleanup, release, or exit fact, or a distinct late Worker event) references a
Job that the smaller bound retired earlier in the fold; restoring the original bound avoids that
configuration-induced refusal. The rule is a function of the records and the bound together. The
previous process's in-memory set can differ from the replayed set only where a durable `job_created`
was never installed in memory, whether it added a resident or replaced one: when `Commit()` returned
`kOutcomeUnknown`, or never returned, and the writer, fail-closed under ADR-0003, latched without
applying it, or when the process crashed after `Commit()` returned `kCommitted` and before
installation. In every such case ADR-0006 Section 3 says the record is committed, so replay folds
it, adds the Job, and retires the victim if the record was a retiring one; the previous process
applied no later writer turn, so the two sets are never both current, although reads and lists of
its last installed set keep working until it stops, as ADR-0008 Section 6 says, and show the old set
once more before the restart shows the new one. The definite-failure case is unchanged: no record is
durable, the victim stays, and replay keeps it too. Replay refuses to start with
`journal_capacity_exceeded` only when a `job_created` finds `max_jobs` Jobs of which none is closed
at that point in the fold. The writer and replay share one implementation of the retirability
predicate and the oldest-first choice, so they cannot diverge. Two rules of Section 1 make the
equality hold: the writer selects the victim at the creation's turn, after every earlier FIFO turn
is applied, so a closing fact queued ahead of the creation (for example the
`cleanup_status_recorded` of an older Job) closes that Job for the writer and for replay alike at
the creation's record; and the writer refuses a creation rather than skipping the oldest closed Job
when a lease blocks it, so a refusal leaves no record and every retirement the writer performed is
the one replay performs at the same record.

Offline pruning (ADR-0006 Section 8) keeps its mechanism, and its eligibility gains the
acknowledgment condition of Section 1: a prefix is prunable only when every Job with a record in it
is closed under Section 1 as folded from the prefix's records alone, so a Job with an
acknowledgment-binding Worker event is prunable only when the prefix also confirms its exit by
either path of Section 1 (a `process_exit_confirmed` record or a failed launch observation);
otherwise the operator operation refuses, and the Job's Worker binding stays replayable with its
open obligation. A pruned
prefix therefore removes only closed Jobs from replay, which only reduces what replay has to
retire, and a pruned Job that the previous process still held resident is absent after replay, as
ADR-0006 already says. Because retirement keeps
every record, a Job retired in one run and still present in the retained Journal is retired again
by replay, at the same point.

This changes two sentences of ADR-0006. In Section 5, the capacity refusal "replay would create
more resident Jobs than the configured maximum" becomes "replay would create more resident Jobs
than the configured maximum after retiring closed Jobs under this ADR's rule". In Section 8, the
prunable condition "every Job with a record in it has a terminal outcome, released resources, and a
recorded cleanup status within the prefix" becomes "every Job with a record in it is closed under
ADR-0014 Section 1 within the prefix". ADR-0006 is superseded in those two sentences only; this ADR
names the superseding paths, and every other rule of ADR-0006 stands.

### 5. The trace

The global trace becomes a ring of `trace_capacity` records. Ordinals are a 64-bit counter that
keeps increasing and never wraps; if it ever reached its maximum, the writer would latch failure as
it does for any exhausted non-wrapping sequence, a bound no process reaches in practice. A turn's
prepared trace is appended to the ring as a whole: when fewer records are free than the turn holds,
the oldest retained turns are evicted whole, one after another, until the turn fits. Whole-turn
eviction makes room only if one turn fits in an empty ring, so the ring has a startup bound distinct
from its occupancy: the writer refuses construction, as it refuses any invalid configuration today,
when `trace_capacity` is smaller than the largest number of records one turn can publish. With the
reducer's closed event and effect sets that is 14 records (the Journal attempt, the commit
observation, and the snapshot activation; two source registrations; two records for each of at most
four effects; and the response release, as `RequiredTraceSlots` in `src/core/job_orchestrator.cpp`
counts them), and the bound is derived from those sets, not configured. The construction rule is
therefore `trace_capacity >= max(total FIFO capacity, 14)`. The existing rule `trace_capacity >=
total FIFO capacity` stays, and under Section 6's reserve `9 * (J + 1) + 1` the smallest valid total
FIFO capacity is 20 (`J = 1`, normal capacity 1), so the existing rule already implies the 14-record
bound in every valid configuration and the composition keeps sizing `trace_capacity` to the total
FIFO capacity; the 14-record check stays explicit so that a later change to either capacity cannot
silently break whole-turn retention. `HasTraceCapacity` becomes a check of one turn's records
against that bound, which cannot fail after a valid construction; ring fullness only ever causes
whole-turn eviction. A reader can rely on these properties: the ordinals of the retained records are
consecutive and end at the most recent one; a record is never reordered or altered; and every
retained turn holds all the records its turn published, because turns are retained and evicted only
whole. A turn that fails at logical commit published its records up to `JournalAttempt` before
`Commit()` was called (the writer order of ADR-0003) and nothing after; those records are retained
as published, the failure latch means no later turn follows, and the trace is not rolled back.
`HasTraceCapacity` no longer fails a turn. The diagnostic ingress-sequence history, which the writer
also preallocates to `trace_capacity` entries and appends to once per admission (including the
shutdown marker and admissions the reducer later rejects), becomes a ring of the same capacity: the
oldest entry is overwritten, its fullness is no longer a `service_failed` cause in ordinary
admission or in the shutdown submission, and `CopyIngressSequences` returns the retained tail in
admission order. The trace and the history are diagnostic test support and no production reader
depends on them.

### 6. The ADR-0003 change

ADR-0003 reserved this decision: "A future Normative deletion/retention decision is required before
successful slots can be reused without changing ADR-0002's terminal-state rejection semantics."
This ADR is that decision. It fills the reserved case and changes ADR-0003 in these places, each
named here as its superseding path:

- "Resident Job bound", the lifetime rule: "Every successfully committed `job_created` holds its
  snapshot and resident slot for the Phase 0A process lifetime" and "Producer quiescence retires
  source gates and permits but does not delete the snapshot or permit slot reuse" become "Every
  successfully committed `job_created` holds its snapshot and resident slot until ADR-0014
  retirement removes the snapshot and resets the slot for reuse (Sections 1 and 2)", and
  "Consequently at most `J` distinct Job IDs can be created successfully in one Phase 0A process"
  no longer holds: `J` bounds the resident Jobs at any time, not the creations in a process.
- "Resident Job bound": "For a new Job ID, it atomically reserves a resident slot and a normal FIFO
  slot before admitting `job_created`" becomes "it atomically reserves a free resident slot, or the
  spare slot with a retirement ticket under ADR-0014 Section 1, and a normal FIFO slot before
  admitting `job_created`". `J` remains the bound on resident Jobs; the spare slot holds only a
  provisional creation.
- The result-table row "New Job with no resident slot, including simultaneous normal-capacity
  exhaustion: `resident_limit`" applies when no slot is free and no ticket can be issued: no
  resident Job is closed in the committed state, or a ticket is already outstanding.
- The turn order "validate and decide" gains, for a creation admitted with a ticket, the outcome
  `resident_limit` in four cases of Section 1: fewer than `max_jobs` Jobs are resident but every
  ordinary slot is resident or still held by a provisional reservation; the selected victim has a
  callback lease in flight; an admitted input for the victim holds a permit queued or in flight; or
  a creation claim on the victim's identifier is outstanding. In each case the creation's completion
  reports `resident_limit`, no reducer decision, Journal sequence, or record is produced, and the
  ticket follows its reservation. It is an ingress-level result delivered through the completion,
  not an ADR-0002 rejection reason.
- The critical reserve bound "`R = 9 * J + 1`" becomes "`R = 9 * (J + 1) + 1`": the spare slot of
  Section 1 is a provisional slot with the per-Job gates of the table, so the reserve counts
  `J + 1` slots; the derivation is otherwise unchanged.
- The gate table gains two causes: while the writer's claim on the victim stands, a lease on the
  victim is refused and the source keeps its fact; and retirement at commit retires every gate of
  the victim. In the Worker row, "Successful ACK or confirmed process exit retires the Worker gate
  and identity" becomes "Successful ACK, confirmed process exit, or retirement under ADR-0014
  retires the Worker gate and identity"; the only sequence a retirement can drop is a post-exit
  `late_worker_event`'s single acknowledgment, because a Job with an open retry obligation is not
  closed (Section 1). The sentence "Once a valid source registration and permit exist, ingress
  performs no second lifecycle-state filter" gains this one exception: while the claim stands, no
  new ownership of the victim's slot is granted (lease, gate permit, or creation claim), because the
  victim is closed and about to be absent; a terminate for it takes the unknown-Job path of normal
  admission, and a `job_created` for its identifier is treated as a new identifier (Section 1).

Separately from ADR-0003, which states no bound on it, the implementation's diagnostic
ingress-sequence history becomes a ring whose fullness is no longer a `service_failed` cause; that
is a change this ADR makes in Section 5, not a superseded ADR-0003 rule.

Every other rule of ADR-0003 stands: the resident-slot allocator as the sole authority for the
bound, the precedence of `resident_limit` over `normal_full`, the provisional reservation rules,
and the closed input classes. ADR-0002 is unchanged: its terminal-state rejection semantics hold
because a retired Job is absent, not terminal, and its inputs are decided by the absent-entity rules
that already exist for pruned Jobs; its acknowledgment rule holds because a Job with an open ACK
obligation is not closed (Section 1). No Journal event is added, so ADR-0002's event set and
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
applying one rule so that the resident set after a restart is the one the retained durable Journal
implies under the same configured `max_jobs` (equal to the set the previous process held after its
last fully installed turn, less any Job offline pruning removed; a durable `job_created` the
previous process never installed, under an unknown commit outcome or a crash before installation,
may add a Job or a retirement, Section 4), and keep
the trace bounded without failing a turn because the ring is full (the ordinal exhaustion of
Section 5 is the only trace-related failure).

Its named checks, Planned until the implementation Issue activates them:

- `job_retirement_oldest_closed_first`: with `max_jobs` residents of which several are closed, a
  creation retires the one with the smallest `job_created` sequence and succeeds; the retired Job is
  absent to `SnapshotFor` and the others are untouched; the victim is still readable until the new
  Job's commit; with a precommit materialization failure injected after the victim is selected, the
  victim stays installed with its gates, no Job is committed, and the ticket is released along the
  fail-closed path; with a logical-commit failure injected the victim stays resident and the writer
  latches; and with every resident permit and the shutdown permit held, a ticketed creation still
  registers its terminate gate and is admitted without `service_failed`;
- `job_retirement_requires_closed`: with `max_jobs` residents of which none is closed (terminal with
  its allocation still committed, which includes every Job with a Worker binding whose exit is
  unconfirmed, such as one terminal through a non-Worker outcome with a delayed Worker event
  recorded as `late_worker_event` and its ACK gate retained; or released but no cleanup record), or
  while a retirement ticket is outstanding, admission of a creation for a different new Job answers
  `resident_limit` and retires nothing, while a `job_created` for the ticketed provisional
  identifier claims that reservation under ADR-0003's same-identifier rules and the writer decides
  `job_already_exists` after the first success; with one closed Job whose callback lease is in
  flight, or for which a critical input was admitted behind the ticketed creation and its callback
  has already returned, the creation is admitted with a ticket and its completion reports
  `resident_limit` with nothing retired, the queued input is processed by its own turn with its
  permit intact, and in every case the creation after the lease, turn, or ticket has finished
  retires that same Job; with several closed Jobs, an ownership blocker (a callback lease, or an
  admitted input holding a permit) on the oldest, and a younger closed Job free of any blocker, the
  ticketed creation's completion reports `resident_limit` with no record instead of retiring the
  younger Job, and the retry after the blocker has cleared retires the oldest; with a `job_created`
  for the closed victim's identifier admitted behind the ticketed creation, the creation reports
  `resident_limit` and that turn is decided `job_already_exists` with the victim's slot intact; and
  with a terminate for the claimed victim submitted while the creation's commit is paused, the
  terminate is admitted through normal admission, the creation commits and retires the victim, and
  the terminate is decided `job_not_found` without touching the reused slot; with the writer paused
  by a non-sleeping barrier after it claims the victim and before commit, a fresh lease request from
  each critical source of the victim is refused, no permit or entry is transferred, and the source
  keeps its fact; and for each refusal or rejection of a ticketed turn (a malformed or
  reducer-rejected first creation, a victim-ownership refusal, and the below-`max_jobs` reserved
  refusal) with a same-identifier successor admitted behind it and a creation for another new
  identifier admitted after that, the ticket stays with the reservation, the other creation answers
  `resident_limit`, the successor performs the ticketed check at its turn and commits with the
  retirement when it is possible, and the ticket is released only then or when the last
  same-identifier claim is released; with a terminal Worker event whose ACK succeeded while the exit
  is unconfirmed, the allocation stays committed, the Job is not closed, and with it as the only
  candidate admission answers `resident_limit`, as replay's Journal-only decision does; with a
  delayed `worker_completed` or `worker_failed` for a closed Job that never had a Worker binding,
  the reducer rejects it, nothing is recorded, and the Job stays closed and is retired; with the
  same delayed events for a Job whose launch failed, the folded snapshot already confirms its exit,
  the Job stays closed, and the ticketed creation retires it; with a Job cancelled or terminated
  before any `resources_committed`, so that no `resources_released` exists, the Job is closed once
  its cleanup status is recorded and is retired oldest-first; and with a Job whose
  `process_exit_confirmed` precedes a distinct `late_worker_event`, the Job is closed and is retired
  oldest-first under capacity pressure even while that event's acknowledgment is still
  unacknowledged, its Worker gate is retired with the slot, and the ACK effect is not dispatched
  again after the reset;
- `job_retirement_not_under_free_slot`: with a free slot, a creation retires nothing, however many
  closed Jobs are resident; when a malformed creation that holds the last provisional slot is
  admitted ahead of a ticketed creation, is rejected at its earlier turn, and no same-identifier
  claim remains, the ticketed creation installs into the freed slot, retires nothing, and releases
  its ticket; and in the order malformed `F`, ticketed `T`, valid same-identifier `F`, all admitted
  before `F` is processed, the first `F` is rejected with the reservation kept for the second, `T`'s
  completion reports `resident_limit` with nothing retired and no record, the second `F` commits
  into its reserved slot, and a retried `T` then retires the oldest closed Job;
- `job_retirement_resets_slot`: after retirement, the victim's slot is empty (banks, prepared banks,
  gate registrations, identity slots, and post-commit mark reset) with every timer-gate generation
  greater than before, the new Job is resident in the former spare slot, and the next retirement
  reuses the reset slot; and with one timer-gate generation of the victim's slot at its maximum, the
  ticketed turn fails before commit, the victim stays resident with its generations unchanged, and
  no `job_created` is committed;
- `job_retirement_stale_timer_rejected`: a timeout notification armed for the retired Job's
  generation cannot acquire its lease and is dropped without a failure latch, and after the reset
  slot is reused by a later creation it is rejected as stale by the generation rule;
- `job_retirement_late_fact_absent`: a Worker, process-exit, resource-release, or cleanup source,
  including a delayed duplicate release or cleanup fact, fails to acquire its lease for the retired
  Job and submits nothing, without a failure latch; a terminate for the retired Job is decided as
  `job_not_found`; nothing is appended;
- `journal_replay_retires_like_writer`: a Journal written by a run that retired Jobs replays to the
  same resident set, with the same residents in the same order, as the run held after its last
  committed turn, including a run in which an older Job's `cleanup_status_recorded` was queued ahead
  of a creation while a newer Job was already closed, so that both the writer and replay retired the
  older Job; including a run in which a ticketed creation was refused because the oldest closed Job
  had an ownership blocker while a younger closed Job was free, so that the retried creation and
  replay both retired the oldest; including a run in which a malformed creation admitted ahead of a
  ticketed creation released its provisional slot before the ticketed turn, so that neither the
  writer nor replay retired anything; and with a retiring `job_created` whose complete record is
  durable while `Commit()` returns `kOutcomeUnknown`, or whose process stops after `kCommitted` and
  before installation, the previous process still holds the victim, and replay of that Journal
  retires the victim and seeds the new Job, which is the asserted restart set; after a prunable
  prefix holding a closed Job that the previous run still held resident is pruned, including a Job
  closed without any allocation, replay of the retained Journal leaves that Job absent and retires
  among the remaining Jobs as before, while a prefix holding a Job whose acknowledgment-binding
  Worker event is followed by no exit confirmation of either kind in the prefix is refused as not
  prunable, and a prefix holding a launch-failed Job with delayed Worker events is pruned because
  the fold confirms its exit; with an ordinary, non-retiring `job_created` durable while `Commit()`
  returns `kOutcomeUnknown` or never returns, the previous process holds the old set and replay adds
  the Job; and replaying one Journal under a larger `max_jobs` keeps Jobs the previous run retired
  while a smaller one retires more or refuses, and a Journal with a post-closure audit for a Job
  that the smaller bound retires earlier is refused as `journal_corrupt` at the smaller bound and
  replays at the original one, which the check records as the configured-bound condition of the
  equality;
- `journal_replay_capacity_after_retirement`: replay refuses with `journal_capacity_exceeded` only
  when a `job_created` finds `max_jobs` Jobs with none closed, and succeeds when one is closed;
- `job_trace_ring_wraps`: more turns than `trace_capacity` succeed, with turns of differing record
  counts; the retained ordinals are consecutive and end at the latest, and every retained turn holds
  all the records it published; and with a logical-commit failure injected on the last turn, the
  trace ends with that turn's `JournalAttempt`, no `JournalCommitted` or later record follows, and
  the earlier retained turns are unchanged; with the ordinal injected near its maximum, the retained
  ordinals stay consecutive up to the maximum and the turn that would exceed it latches failure; and
  with more admissions than `trace_capacity`, counting reducer-rejected cancels for unknown Jobs,
  admission keeps succeeding, the ingress-sequence history holds the latest `trace_capacity`
  sequences in order, and the shutdown marker is still admitted; and after a retirement, the records
  the retired Job's turns published are still retained with consecutive ordinals until whole-turn
  eviction removes them; at the smallest valid configuration (`max_jobs` 1, normal capacity 1,
  `trace_capacity` equal to the total FIFO capacity of 20), a `terminal_outcome_committed` turn with
  four effects is retained whole and evicted whole by the next turn that needs its room; a
  configuration with `trace_capacity` one below the total FIFO capacity is refused at construction;
  and the 14-record per-turn bound is asserted against the reducer's closed event and effect sets;
- `rest_v1_retired_job_absent`: after a retirement through the HTTP layer, `GET` of the retired Job
  is `404` `job_not_found`, the list omits it and lists the surviving older Jobs before the new Job
  in `job_created` order, and a cancel of it is `404`; and
- `rest_v1_daemon_outlives_max_jobs`: through the HTTP layer, more than `max_jobs` Jobs are created
  and closed in one process without a `capacity_exhausted` answer; and a create that answered `503`
  `capacity_exhausted` while an admitted input for the oldest closed Job held its permit succeeds
  when retried after that input's turn.

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
| 6. The ADR-0003 change | 6 | no new check; the ADR-0003 and ADR-0002 checks stay active, and `job_ingress_capacity_and_reserve` keeps its name with its expected reserve updated to `9 * (J + 1) + 1` when the implementation Issue activates this ADR; the saturated spare-slot vector of `job_retirement_oldest_closed_first` covers the new permit |
| 7. Tests | this section | all of the above |

## Consequences

- Good: a daemon's lifetime is no longer bounded by `max_jobs` creations or `trace_capacity` turns;
  it serves Jobs as long as closed Jobs can be retired and the ADR-0013 identity source has room.
  That source keeps every identity it issued, including those of retired Jobs and of refused
  creations (three per creation), and latches the core when its capacity of 1,048,576 by default is
  full, so the daemon is restarted at that point, as ADR-0013 Section 2 says.
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
  in flight or an admitted input queued or in flight, or another retirement ticket is outstanding,
  although a closed Job exists; a later creation succeeds once those blockers have cleared and a
  closed Job is still resident. The refusal keeps the writer's choice equal to replay's and leaves
  every admitted entry owned until its own turn.
- Bad: a Job with an open acknowledgment obligation (a terminal or late Worker event recorded, its
  process exit never confirmed) is not closed and holds its slot for the process lifetime;
  ADR-0002's ACK rule is kept rather than cancelled by retirement.
- Neutral: a creation's completion can report `resident_limit`, and the allocator holds one spare
  slot beyond `max_jobs`; one sentence of ADR-0008's Decision is superseded; the diagnostic
  ingress-sequence history becomes a ring like the trace.
- Neutral: two sentences of ADR-0006 are superseded (the Section 5 capacity refusal and the Section
  8 prunable condition); the Journal format and the pruning mechanism are untouched.

## Options considered

- **Retire a Job as soon as it closes**: rejected because a completed Job would be absent to the
  first `GET` after its last cleanup record, which turns a routine client flow into a race, while
  capacity-pressure retirement gives the same lifetime guarantee.
- **Retire after a retention period**: rejected because it needs the timer port, which is not
  designed (H11), and because a time-based rule cannot be reproduced by replay from the Journal.
- **Record retirement as a Journal event**: rejected because it adds an event to ADR-0002's closed
  set and an input class to ADR-0003, two superseding ADRs for a fact the Journal already implies.
- **Select the victim in the ingress allocator at admission**: rejected on 2026-10-29 because the
  allocator sees only committed state while replay sees every record before the creation, so a
  closing fact queued ahead of the creation would make the writer and replay retire different Jobs.
- **Retire a Job whose ACK obligation is open and cancel the retries**: rejected on 2026-10-30
  because it would supersede ADR-0002's acknowledgment rule for the rare case of a Job whose exit is
  never confirmed; keeping such a Job resident costs one slot and no protocol change.
- **Let the source of ADR-0013 forget retired identifiers**: rejected on 2026-10-27 under Issue
  #102; the remembered set is kept for the source instance's lifetime.
- **Defer H7 to Phase 2**: rejected because the daemon would accept at most `max_jobs` creations per
  process, which is tolerable only while no Application is registered.
- **Keep the append-only trace and fail turns when it is full**: rejected because it bounds the
  daemon's lifetime by `trace_capacity` turns for a diagnostic structure no production reader uses.

## References

- Issue #103
- Gate #101 (P1-7); Gate #50 finding H7; Issue #73
- Requirements: `OPS-006` (Normative)
- Contract Registry: `Resident Job retirement and daemon lifetime`; `Single-state-writer ingress
  and critical reserve` (the reserved case is filled); `JobJournal startup validation, replay, and
  pruning` (one sentence superseded)
- Related ADR: ADR-0002, ADR-0003, ADR-0006, ADR-0008, ADR-0012, ADR-0013
- `spike/README.md`, finding 4
