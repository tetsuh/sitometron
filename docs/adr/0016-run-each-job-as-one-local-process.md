# ADR-0016: Run each Job as one local process

## Status

Proposed — 2026-11-07

## Context

ADR-0005 defines `ApplicationRunnerPort` with three handoffs, `HandoffLaunch`,
`HandoffCooperativeStop`, and `HandoffForcedStop`, and fixes what they carry:
"`ApplicationLaunchRequest` deliberately adds only `job_id` to the exact accepted launch-intent
payload. It does not add executable paths, commands, environment variables, credentials, Session
APIs, resolved-allocation interpretation, or proprietary Application data. Stop requests contain
only the three existing bound identities." The port "performs no synchronous operational result,
callback, ingress submission, retry, deduplication, once-only decision, or Worker stop-and-wait
logic. Launch observations and process-exit outcomes return later as independently admitted raw
candidates." Only a placeholder implements it: the Phase 1 composition discards every handoff
(`apps/sitometrond/composition.cpp`).

The Accepted contracts already fix what a runner must and must not do:

- ADR-0012 Section 1 assigns `worker_launch_observed` and `process_exit_confirmed` to "the
  application runner" and `worker_running`, `worker_completed`, and `worker_failed` to "the Worker
  protocol". Its Section 7 leaves open "What a process without a Worker protocol reports, and the
  relation between `worker_completed` and `process_exit_confirmed` for it".
- ADR-0002 and the reducer contract decide every input the runner submits. `worker_launch_observed`
  is accepted only in `preparing`; `failed` enters `finalizing` with `completion_mode`
  `process_already_exited` and the exit confirmed. `process_exit_confirmed` in `preparing` or
  `running` is an unexpected exit that latches `failed`; in `stopping` it enters `finalizing` and
  keeps the mode the stop set; in `finalizing` or a terminal state its first confirmation records
  the payload's mode. The payload carries the launch operation ID, "so stale or misrouted process
  observations cannot terminate another launch". ADR-0002 also fixes "Cancel or terminate accepted
  before a Worker process exists enters `finalizing` directly", and that once a process may exist,
  cancel requests cooperative stop and terminate requests forced stop.
- ADR-0003 classifies `worker_launch_observed` as a normal input, which can be refused with
  `normal_full`, and `process_exit_confirmed` as a critical input whose process-exit gate "is
  registered" by the writer "after the launch binding exists but before process start exposes a
  callback". Callbacks "hold a lifetime-safe ingress handle rather than a raw writer or controller
  pointer", and "each preallocated callback registration permits at most one active callback
  invocation; its producer or adapter serializes that registration".
- ADR-0005 requires that, after entry into a handoff, "the implementation may only move into that
  already prepared storage and must catch no exception because it performs no potentially throwing
  copy, allocation, or synchronous external operation". The writer calls the handoffs on its own
  thread after commit (`DispatchPrepared` in `src/core/job_orchestrator.cpp`), so the walking
  skeleton's finding 5 applies: a handoff must not block or re-enter ingress (`spike/README.md`).
- ADR-0015 Section 4 gives the runner the verified registry set and says "the runner resolves a
  launch request's `application_id` to the entry's `executable` and `arguments` itself"; Section 6
  leaves the working directory, the environment, the identities the process receives, and
  re-verification of the digest at launch to this decision.
- ADR-0014 keeps a Job with an open acknowledgment obligation resident "until its exit is
  confirmed", so a runner that never confirms an exit holds a slot for the process lifetime.
- The reducer arms and disarms the preparation, execution, cooperative-stop, and
  process-exit-confirmation timers through effects; only the timer port (Gate #123 P2-5, Issue
  #128) fires `timeout_expired`. A timeout then reaches the runner only as a stop handoff.

The walking skeleton ran one POSIX child per Job with `posix_spawnp`, `kill`, and `waitpid`, and
submitted from one thread per Job directly into the orchestrator. It had no Windows path, no forced
stop, and no timer (`spike/README.md`, "Intentionally missing"). It also allocated inside
`HandoffLaunch`, which ADR-0005 forbids.

Gate #123 scopes this to "one process per Job on Linux and Windows; containment of descendants is
out of scope (see P2-9)".

The owner decided on 2026-11-07, as recorded in Issue #126: on Windows, Phase 2 carries a
cooperative stop out with `TerminateProcess`; the runner does not verify the executable digest again
at launch; at daemon shutdown the runner delivers a forced stop to every live process at once; a
failed-launch observation that closed admission never accepts gets no further runner fact; and a
live process is supervised until it exits or the runner is stopped, not stopped on the fail-closed
latch.

## Decision

We will run each Job as one local process in a new adapter, `sitometron_process`, that consumes
the three handoffs into bounded per-Job storage, starts the registered executable from its own
threads, delivers stops, observes the exit, and submits exactly `worker_launch_observed` and
`process_exit_confirmed` through a lifetime-safe ingress handle.

### 1. What is started

For a launch request, the runner resolves `intent.application_id` to the launch specification the
composition built for it at startup from the verified registry entry (ADR-0015 Section 4): the
`executable`, the `arguments`, and the `bundle_sha256`, copied into values the adapter owns. It
starts `executable` with `arguments` in order, as separate argument strings and without shell
interpretation. The registered `arguments` are the parameters, not the whole vector: the process
sees the `executable` path as `argv[0]` and the registered strings as `argv[1]` onward, so
ADR-0015's example `["--mode", "batch"]` arrives as `argv[1]` and `argv[2]`, and an empty
`arguments` array gives `argv[0]` alone. On Windows, where `CreateProcessW` takes one command line,
the runner serializes the `executable` path followed by the arguments with the convention that
`CommandLineToArgvW` and the C runtime parse: an argument is quoted when it is empty or contains a
space, a tab, or a quotation mark; a quotation mark inside it is escaped with a backslash; and
backslashes that precede a quotation mark or the closing quote are doubled. A child that reads
`argv` from the C runtime or calls `CommandLineToArgvW` receives the registered strings unchanged; a
child that parses its command line by another rule is outside this guarantee. The process inherits
the daemon's environment and runs in the daemon's current working directory. The runner adds these
environment variables, overriding any inherited value:

| Variable | Value |
|---|---|
| `SITOMETRON_JOB_ID` | the Job identity (`job_id`), canonical UUID text |
| `SITOMETRON_WORKER_ID` | the pre-generated Worker identity (`intent.worker_id`) |
| `SITOMETRON_LAUNCH_OPERATION_ID` | the launch operation identity (`intent.operation_id`) |

The Worker protocol (Issue #127) adds the variables a Worker needs to report, in the same
`SITOMETRON_` namespace; this ADR reserves the prefix and adds nothing else.

The runner does not verify the digest again at launch: ADR-0015 verifies every `executable` once at
startup, and this ADR leaves the window between that verification and a launch open. A file replaced
while the daemon runs is started as it is. On any failure before the operating system has created
the process, such as a missing or unreadable executable, the runner starts nothing and submits
`worker_launch_observed{failed}`. Once the operating system has created the process, the runner
submits `worker_launch_observed{started}`; a process that then exits at once, for example because
the executable could not be loaded, is reported by the ordinary exit path.

### 2. What is submitted, and when

The runner submits exactly two event kinds:

- `worker_launch_observed{operation_id, started|failed}` at most once per launch request, after the
  start attempt;
- `process_exit_confirmed{launch_operation_id, completion_mode}` once per process it created, after
  the process has exited, and once per launch request that ends without a process under the rules
  below.

A failed launch that the reducer accepts submits no `process_exit_confirmed`: ADR-0002 already
records the exit as confirmed with `process_already_exited`.

`completion_mode` names the strongest stop the runner was asked to deliver to that process before
its exit: `forced` if a forced stop was handed off, otherwise `cooperative` if a cooperative stop
was handed off, otherwise `process_already_exited`. It names the request, not the platform action,
so a cancelled Job records `cooperative` on both platforms. ADR-0002 uses the payload only on a
first confirmation in `finalizing` or a terminal state and keeps the mode a stop or timeout set in
`stopping`; the runner reports its own observation in every case.

A Job can leave `preparing` before the runner reports the start: a cancel, a terminate, or the
preparation timeout moves it to `stopping` while its process presence is `unknown`, and only
`process_exit_confirmed` or a Worker terminal event leaves `stopping`. The runner therefore
guarantees one exit report for every launch request it did not report `failed` with acceptance:

- a stop handoff that arrives before the start cancels the launch (Section 3), and the runner
  submits `process_exit_confirmed` with the stop's mode;
- a `worker_launch_observed{failed}` that the reducer rejects because the Job is no longer in
  `preparing` is followed by one `process_exit_confirmed` with the strongest stop mode handed off,
  otherwise `process_already_exited`;
- a `worker_launch_observed{started}` that the reducer rejects for the same reason leaves the
  process live: the runner delivers the stops it holds and reports the exit as for any process.

Every other reducer rejection is final: the runner submits no input twice. The exceptions to the
guarantee are the two cases in which normal admission has closed under the Job: a launch that the
stop flag of Section 5 cancels before its spawn, and a `failed` observation whose retries end with
`admission_closed` or `service_failed` (Section 4). Neither gets a further runner fact; the Job
stays as committed for the next start, where replay reports it unresolved (ADR-0006).

A Worker may be ready to report before the runner's `worker_launch_observed{started}` is committed,
for example while that observation is parked by `normal_full`, and the reducer refuses
`worker_running` until the launch is observed. The Worker protocol (Issue #127) decides how its
adapter holds or retries a report until the committed snapshot shows the launch observed; the
runner's only obligation is to submit the observation as soon as the operating system has created
the process, before anything else for that Job. The runner produces no Worker protocol event. A
process that reports nothing through the Worker protocol therefore never enters `running`: if it
exits, the runner's `process_exit_confirmed` in `preparing` is the unexpected exit of ADR-0002 and
the Job ends `failed`; if it does not exit, the preparation timeout ends it. This answers the
question ADR-0012 Section 7 left open: in Phase 2 a process without the Worker protocol cannot
succeed, and `worker_completed` and `process_exit_confirmed` stay two facts from two producers.
Changing that assignment needs a superseding ADR for ADR-0012 Section 1.

The runner does not interpret the exit status: no Journal event carries it, so it goes only to the
daemon's diagnostics.

### 3. Stops

A cooperative stop asks the process to finish; a forced stop ends it. On Linux the two differ; on
Windows, Phase 2 has no portable way to ask a process to finish without an agreement with the
Worker, so a cooperative request is carried out by ending the process, and a Windows Worker cannot
clean up before it stops. Only the process the runner created receives a stop, never its descendants
(Gate #123 P2-9).

| Platform | Cooperative stop | Forced stop |
|---|---|---|
| Linux | `SIGTERM` to the process | `SIGKILL` to the process |
| Windows | `TerminateProcess` (Phase 2 carries the request out by ending the process) | `TerminateProcess` |

A stop handoff that the slot thread finds set before it starts the process cancels the launch: the
runner starts nothing and submits no `worker_launch_observed`; Section 2 gives the exit report. A
stop that arrives while the spawn is in progress is delivered to the new process as soon as it is
published (Section 4). A stop for a process that has already exited, or for a launch that ended
without a process, is ignored. A forced stop after a cooperative stop is delivered; a second stop of
the same kind is not. The service thread delivers stops (Section 4), and the runner never signals a
process identifier it has already reaped.

The runner arms no timer and waits for no timeout or grace period of its own; its only wait is for a
process to exit (Section 4). The reducer's timeout effects and the timer port decide when a
cooperative stop escalates to a forced one, and that decision reaches the runner as
`HandoffForcedStop`.

### 4. Handoffs, storage, and threads

The adapter has two objects. The port object implements `ApplicationRunnerPort` and owns nothing
but a fixed table of mailboxes, `max_jobs + 1` of them (ADR-0014 Section 1), allocated at
construction. The producer object owns the threads, the launch specifications, the ingress handles,
the parking table, and the capacity waiter of this section; it drains the mailboxes. The port
retains no reference to the producer, the writer, or ingress (ADR-0005).

Each mailbox has two cells. A cell is empty or holds one Job from its launch handoff until its
release: the launch request, two stop flags, one per kind, the process that may be stopped, and a
phase, `active` until the slot thread submits the Job's last fact, `process_exit_confirmed` or
`worker_launch_observed{failed}`, and `closing` from the moment before that submission until the
thread has taken the Job's completions and met the obligations of Section 2. A Job never moves
between cells: its stop flags stay where a stop can find them for its whole stoppable lifetime, and
the slot thread serves its mailbox's cells in the order they were claimed. A cell is emptied only at
release.

A handoff takes no lock. Each cell has one atomic word that packs a generation, the occupied bit,
the phase, and the two stop flags; the Job identity of an occupied cell is written before the word
that publishes it. A launch claims, with a compare-and-exchange on that word, an empty cell of a
mailbox whose other cell is empty or `closing`, never of a mailbox whose other cell is `active`, and
publishes the moved request with a release store. A stop reads every cell of every mailbox, sets the
flag of the cell whose Job it names with a compare-and-exchange that carries the generation it read,
and repeats the scan once if that exchange fails because the cell changed; a Job is in one cell for
its whole lifetime, so a stop that finds it in no cell on a scan that read every cell after the
Job's launch handoff was published has found a released Job, and is ignored, as Section 3 ignores a
stop for a Job whose last fact was submitted. A release empties a cell with an exchange to the next
generation, so a stale stop never lands on the cell's next occupant. A repeated stop of the same
kind sets nothing new. A handoff allocates nothing, blocks on no I/O, throws nothing, and never
calls back into the core.

A launch always finds a place: the writer holds at most `max_jobs + 1` Jobs with a launch binding,
and the Job being launched is one of them, so at most `max_jobs` others have one. A retired Job's
cell is `closing`, because its last fact was submitted before it could be committed, installed, and
the Job closed and retired (ADR-0014 Section 1); the `closing` phase is entered before the
submission, so this holds even while the slot thread is descheduled with the submission in flight.
Therefore every mailbox with an `active` cell holds one of the at most `max_jobs` other Jobs, and
every other mailbox offers its empty cell unless a different one of those Jobs already claimed it;
`max_jobs` Jobs cannot take all `max_jobs + 1` places. The three-Job interleaving with `max_jobs =
1`, in which two Jobs retire while their threads are descheduled before their submissions return,
finds the third Job a place in the `closing` cell's partner. The producer treats a launch that finds
none as an invariant violation: the handoff records it, and the service thread, outside any handoff,
reports it on standard error and latches the core's fail-closed state through a Sitometron-owned
failure interface the composition builds on the core's internal latch (ADR-0003), so readiness
becomes false and admission closes; the affected Job and every other open Job stay as committed for
the next start (ADR-0006). No Journal fact describes the violation.

Work happens on the producer's threads, all created at construction:

- one slot thread per mailbox, `max_jobs + 1` of them, each serving its mailbox's cells one at a
  time in claim order: it takes the cell's launch, checks the stop flags under the mailbox lock and
  cancels the launch if one is set (Section 3), otherwise starts the process and publishes it under
  the lock, submits `worker_launch_observed` and takes its completion at once, waits for the exit
  without reaping it, marks the process gone under the lock, reaps it, marks the cell `closing`,
  submits `process_exit_confirmed`, takes that completion, meets the obligations of Section 2,
  releases the cell, and turns to the other cell if it is occupied;
- one service thread, woken by handoffs and the stop request, that delivers every stop (Section
  3) to the published process of the mailbox the stop names;
- one relay thread that waits on the core's normal-capacity notification and resubmits every
  `started` observation that a `normal_full` answer parked, until it is answered otherwise or the
  Job's exit has been observed, in which case the parked observation is dropped and only
  `process_exit_confirmed` is submitted.

No thread is created after construction, so no launch can fail for want of a thread and no thread
outlives its mailbox. Each thread submits through its own lifetime-safe ingress handle, obtained by
the composition from the core (ADR-0003), so each registration has one invocation at a time and
`max_jobs + 3` registrations serve the runner; each slot thread and the relay thread is also a
registered capacity waiter. A thread takes the completion of each submission it made, from that same
thread, through the handle, before it makes another submission or waits for an exit, so the runner
holds at most one completion per thread at any time and a running process holds none. The completion
carries the reducer result that Section 2 depends on.

A `normal_full` answer to a `started` observation parks only the observation: the slot thread
records it in the producer's parking table, one entry per mailbox, and goes on to wait for the exit.
The relay thread resubmits it and takes its completion, so no exit report, stop delivery, or other
Job is delayed by it. A `normal_full` answer to a `failed` observation is retried by the slot thread
itself, which has no process to wait for: it waits on the capacity notification as a registered
waiter of its own and resubmits, so the reducer result that decides the exit obligation returns to
the thread that owes it, and the mailbox stays `busy` until that obligation is met.
`admission_closed` and `service_failed` end the retries.

The capacity notification is per waiter: the core keeps one level-triggered flag for each registered
waiter, sets every flag when a writer turn frees a normal slot and when it closes admission or
latches failure, and lets each waiter clear only its own, so the supervisor's drain (ADR-0012
Section 4) and the Session retainer placeholder (Issue #133) never consume a notification meant for
the runner. The producer can set the flag of each of its own waiters too, and does so at its stop. A
waiter clears its flag before it scans for work and waits only when the scan found none and the flag
is still clear, so a flag set between the scan and the wait is not lost. A slot thread that parks an
observation sets the relay's flag after writing the parking entry, so the relay rescans the table
even if the capacity event that freed the slot came before the park; and every waiter rechecks the
producer's stop flag whenever it wakes. The handles, the waiter, and the failure interface are
Sitometron-owned interfaces the composition builds on the core's internal API; the producer never
holds the orchestrator.

Each stop flag has a matching delivered bit in the same atomic word. A stop is delivered under the
mailbox lock by whichever thread first finds its flag set, its delivered bit clear, and a process
published: the service thread on its wake or the slot thread when it publishes the process. The
delivering thread sets the delivered bit under the lock before it calls the operating system, so
each flag is delivered exactly once even when both threads see it, and a flag set while the spawn is
in progress is delivered by the slot thread immediately after publication. When both flags are
pending and undelivered, the cooperative stop is delivered first and the forced stop immediately
after it. The service thread takes the lock, checks that the process is published and not marked
gone, signals the process identifier (Linux) or calls `TerminateProcess` on the handle (Windows),
and releases it; the slot thread observes the exit without reaping, with `waitid` and `WNOWAIT` on
Linux and `WaitForSingleObject` on Windows, then takes the lock, marks the process gone, releases
it, and only then reaps with `waitpid` or closes the handle. So a signal never reaches a reused
process identifier, a stop recorded after the mark is ignored, and no stop is lost between the check
before the spawn and the publication. The lock is never held during a wait, a spawn, or a
submission.

### 5. Shutdown and failures after the start

When the composition stops the runner, the producer sets its stop flag and the flag of every
capacity waiter it owns, so an idle relay thread and a slot thread retrying a `failed` observation
wake and stop waiting, and the service thread delivers a forced stop to every published process. A
slot thread that finds the flag set before its spawn cancels the launch and submits nothing; one
that finds it set when it publishes a process delivers the forced stop itself; a launch still
waiting in a cell is dropped. Each slot thread then waits for its exit, reaps it, and takes or
abandons its last completion; the producer joins every thread before it returns. It delivers no
cooperative stop first: a Job open at shutdown is left as committed (ADR-0012), so nothing waits for
its result.

A slot thread keeps waiting for and reaps its process whatever the writer does. After a fail-closed
latch or a Journal failure, a submission is answered `service_failed` or `admission_closed`, the
runner submits nothing more for that Job, and a live process is supervised as before: it is not
stopped because of the latch, it receives the stops handed off before the latch and the forced stop
of the runner's shutdown, and it is reaped when it exits. The Job stays unresolved for the next
start (ADR-0006). The daemon never exits with a process it created still unreaped.

### 6. Placement and dependencies

The runner lives in `sitometron_process`, a static library under `src/process/` with
Sitometron-owned public headers under `include/sitometron/process/`. Its public headers expose the
port object as an implementation of `core::ApplicationRunnerPort`, the producer object, the launch
specification as a struct of standard strings and vectors that the composition fills from each
verified registry entry, and the two Sitometron-owned interfaces of Section 4 that the composition
implements. The adapter includes `<sitometron/core/job_ports.hpp>` and
`<sitometron/core/job_reducer.hpp>` for the port and the DTOs, and no header of another adapter.
`sitometron_registry` is not changed. `sitometron_core` gains no public API; the implementation
Issue extends the core's internal capacity notification to one flag per registered waiter, as
Section 4 states, without changing a rule of ADR-0012. The composition constructs the port and the
producer after the core and before any listener opens, with `callback_registration_capacity` sized
for the `max_jobs + 3` handles of Section 4 and every other producer, and `completion_capacity`
raised by `max_jobs + 3`, the most completions the runner can hold untaken at once (one per thread),
on top of what the other producers can hold, so a critical submission never finds completion storage
exhausted because of the runner.

`sitometron_process` links no third-party target and adds no manifest port; its only dependencies
are the standard library and the platform process APIs below, so its allowlist row in the format of
ADR-0010 Section 1 is empty.

Platform process APIs are the adapter's own: on Linux `posix_spawn`, `kill`, `waitid`, and `waitpid`
through `<spawn.h>`, `<signal.h>`, and `<sys/wait.h>`; on Windows `CreateProcessW`,
`TerminateProcess`, and `WaitForSingleObject` through `<windows.h>`. The implementation Issue lists
the exact standard and platform headers in the target's allowlist and adds the dependency-allowlist
and public-API-isolation scanners in the pattern of the `sitometron_registry` scanners. The target
is built only when HTTP is enabled, like `sitometrond_composition`, its only consumer.

### 7. What changes in the Accepted ADRs

- [ADR-0005](0005-define-phase-0a-core-capability-port-contracts.md): unchanged. The port interface,
  the request contents, and the handoff rules are implemented, not altered.
- [ADR-0012](0012-drive-each-job-with-a-supervisor-in-the-core.md) Section 7: the item "What a
  process without a Worker protocol reports" is decided here (Section 2); Section 1's producer table
  is unchanged. Section 4's sentence "After any turn that frees a normal slot, the writer sets one
  daemon-wide capacity signal" is supplemented, not replaced: the writer still sets that signal,
  which only the supervisor drains, and in addition sets one flag per other registered waiter
  (Section 4). This is the one Accepted-ADR sentence this ADR changes; the supervisor's behavior
  under it is unchanged.
- [ADR-0015](0015-register-applications-with-a-verified-registry-file.md) Section 6: the runner's
  inputs are decided here (Section 1), and re-verification at launch is decided against; no sentence
  is superseded.
- [ADR-0002](0002-define-core-job-reducer-contract.md),
  [ADR-0003](0003-define-single-state-writer-ingress-contract.md), and the reducer contract:
  unchanged.

### 8. Not decided here

- Timer durations and the timer port (Issue #128), and what "if needed" means for the
  process-exit-confirmation timer (#107).
- The Worker protocol, the variables a Worker uses to report, the acknowledgment, and how its
  adapter orders a Worker's first report after the committed launch observation (Issue #127).
- Containment of descendants, job objects, and cgroups (Gate #123 P2-9).
- A per-Job working directory, the Worker's output, and Artifacts (Phase 5).
- Resource placement and affinity (Phase 3).
- Resolving Jobs that replay leaves unresolved, including a process whose state is unknown after a
  restart (ADR-0006; #35, #73).

## Requirements and checks

This ADR adds `WRK-001` (MUST): run each Job as one local process started from its verified registry
entry with the identities of Section 1 in its environment, deliver cooperative and forced stops as
Section 3 states, and submit exactly `worker_launch_observed` and `process_exit_confirmed` as
Section 2 states, from storage and threads that never block, allocate, or re-enter the core inside a
handoff.

Its named checks, Planned until the implementation Issue activates them. Process checks start a
test child program that waits on the stop mechanism of its platform and exits with a requested
status; no check sleeps, and every ordering is established through the runner's submissions and the
existing non-sleeping barriers:

- `process_launch_observed_after_start`: a registered executable is started with its `arguments` at
  `argv[1]` onward in order and the `executable` path at `argv[0]`, also with an empty `arguments`
  array, in the daemon's current working directory, with a sentinel variable of the daemon's
  environment preserved and the three identity variables set even where the daemon's environment
  carried conflicting values; `worker_launch_observed{started}` follows the start, and the child
  reports the arguments, the variables, and its working directory back through an output file; the
  round trip on both platforms covers an empty argument string and an argument with spaces, and on
  Windows also one with quotation marks, one ending in a backslash, and one with a backslash before
  a quotation mark;
- `process_failed_start_reports_failed`: a missing executable and an unreadable executable each
  submit `worker_launch_observed{failed}`, start nothing, and submit no `process_exit_confirmed`; an
  executable replaced after startup with a different file is started, because the runner does not
  verify the digest again;
- `process_exit_confirmed_mode`: a process that exits on its own reports `process_already_exited`;
  after a cooperative stop, `cooperative`; after a forced stop, `forced`, also when a cooperative
  stop preceded it; the launch operation ID is the request's;
- `process_stop_before_start_skips_launch`: a stop handed off before the start cancels it, with no
  `worker_launch_observed` and one `process_exit_confirmed` carrying the stop's mode;
- `process_stop_delivery`: on Linux `SIGTERM` then `SIGKILL`, on Windows `TerminateProcess` for
  either request, each delivered once; a repeated stop of the same kind is not delivered; a stop
  after the exit is ignored; the child's descendants are not signaled;
- `process_handoff_bounded`: `max_jobs + 1` concurrent launches never allocate or lock inside a
  handoff, a launch that finds no place latches the fail-closed state from the service thread and
  leaves every Job as committed, a launch for a slot's next occupant after retirement takes the
  partner cell of the `closing` one while its thread is still taking the old Job's completions and
  starts once it is done, the three-Job interleaving with `max_jobs = 1` and both threads
  descheduled with their last submissions in flight finds the third launch a place, a cooperative
  and a forced stop handed off before the drain are both delivered in order, a stop for a queued
  launch cancels it when its slot thread turns to it, a stop handed off at the same time as the
  release or the reuse of a cell, or between the two reads of its scan (driven through a
  deterministic interleaving at the compare-and-exchange and at the scan), still reaches its Job if
  its last fact is not yet submitted and never reaches the cell's next occupant, a stop for a Job
  without a mailbox is ignored, no thread is created after construction, the runner holds at most
  one completion per thread while a process runs, and a handoff never re-enters ingress; the check
  runs under the ASan/UBSan configuration and, locally, TSan;
- `process_normal_full_waits`: a `normal_full` answer to `worker_launch_observed` parks the fact in
  its mailbox while that Job's slot thread, another Job's exit report, and a stop delivery all
  proceed; the relay thread is woken by its own capacity flag without polling, including a slot
  freed before the park, a slot freed between the relay's scan and its wait, and a slot whose
  notification the supervisor drained first, and the fact is then accepted; a `failed` observation
  that got `normal_full` is retried by its slot thread on its own capacity flag and its obligation
  is met before the mailbox leaves `busy`; after the exit only `process_exit_confirmed` is
  submitted;
- `process_exit_reported_after_rejected_observation`: a launch observation whose completion reports
  a rejection because the Job left `preparing` is followed by exactly one `process_exit_confirmed`
  with the mode Section 2 gives, and a process that was live is stopped and reported; a stop handed
  off while the slot thread waits for the exit reaches the process without waking that thread, a
  stop handed off while the spawn is in progress reaches the new process once it is published and is
  not delivered a second time by the service thread, and a cooperative and a forced stop both
  pending at publication are delivered in that order, each once;
- `process_reaped_after_failure`: a process whose `process_exit_confirmed` is answered
  `service_failed` or `admission_closed` is still reaped; a live process whose
  `worker_launch_observed` is answered `service_failed` keeps running until it exits or the runner
  is stopped, and is then reaped; a `failed` observation whose retries end with `admission_closed`
  gets no `process_exit_confirmed` and its Job stays as committed; shutdown with a live process,
  with a spawn in progress, with a launch not yet spawned, with a launch still waiting in a cell,
  with an idle relay thread waiting for capacity, and with a slot thread retrying a `failed`
  observation, and closure of admission by the core without another capacity event, each delivers a
  forced stop to each process that exists, wakes every waiter, starts nothing new, submits no fact
  for a launch cancelled before its spawn, reaps every process, and joins every thread;
- `process_dependency_allowlist` and `process_public_api_isolation`: the scanners of Section 6, with
  planted violations;
- `rest_v1_protocol_less_process_fails`: through production `sitometrond` with a verified registry,
  a create for an Application whose process exits without reporting reaches `failed` with its exit
  confirmed and a recorded cleanup status, on Linux and Windows; activated by the runner
  implementation Issue, which merges after the supervisor implementation (Issue #133).

## Consequences

- Good: the first production Job runs as a real process on both platforms, with the identities a
  Worker needs to report and the two facts the reducer expects from a runner.
- Good: the handoff rules of ADR-0005 are implemented with bounded storage sized by the same
  `max_jobs + 1` the writer uses, so a launch cannot be lost for lack of space.
- Bad: the digest is verified at startup only, so an executable replaced while the daemon runs is
  started as it is.
- Bad: a process without the Worker protocol cannot succeed in Phase 2; the success path needs Issue
  #127.
- Bad: on Windows a cooperative stop ends the process, so a Windows Worker cannot clean up before it
  stops; a later ADR can add a cooperative mechanism when a Worker needs one.
- Neutral: a new adapter target with its own allowlist and scanners, in the existing pattern.

## Options considered

- **Let the runner produce `worker_running`, `worker_completed`, and `worker_failed` for a
  protocol-less process**, as the walking skeleton did: rejected because ADR-0012 Section 1 assigns
  those events to the Worker protocol, and because a process that cannot report cannot say that it
  succeeded.
- **Verify the digest again immediately before the start**: rejected on 2026-11-07 because it costs
  time proportional to the executable on every launch, adds a hashing dependency to the adapter, and
  still leaves a window between the hash and the start.
- **Cooperative stop on Windows through a console control event**: rejected on 2026-11-07 because it
  needs the daemon and the process to share a console, which a daemon run as a service does not
  have, and because it reaches every process in the group.
- **Cooperative stop on Windows through an inherited event handle**: rejected on 2026-11-07 because
  it needs an agreement with every Worker and a variable in the environment for a mechanism Phase 2
  does not use.
- **Stop a live process as soon as the fail-closed latch is set**: rejected on 2026-11-07 because
  the runner would need to learn of the latch through a new notification, and the daemon is already
  unready and restarted by the operator, who then resolves the Job.
- **Submit `process_exit_confirmed` for a failed launch whose observation was never accepted**:
  rejected on 2026-11-07 because the supervisor drives nothing once admission has closed, so the Job
  would stay unresolved either way, and one shutdown rule is simpler than two.
- **A cooperative stop with a grace period at shutdown**: rejected for Phase 2 because a Job open at
  shutdown is left as committed anyway (ADR-0012), and the grace period would add a duration to a
  shutdown that ADR-0003 keeps count-bounded.
- **Submit directly into the orchestrator from the slot threads**: rejected because ADR-0005 forbids
  a port to hold the writer or ingress, and ADR-0003 requires a lifetime-safe handle with one active
  invocation per registration.
- **One submitting thread for every Job**: rejected because a completion wait or a `normal_full`
  park on it would delay every other Job's exit report, which is a critical input; each slot thread
  submits and waits for its own Job instead, with its own registration.
- **One thread created per launch**: rejected because thread creation can fail after the handoff,
  which needs a failure path with no process, and because a retired Job's thread can outlive its
  slot's reuse without bound, which defeats every storage and registration proof; a fixed thread per
  mailbox has neither problem.
- **Wake the waiting slot thread to deliver a stop**: rejected because the service thread can signal
  a process identifier or call `TerminateProcess` on a handle while another thread waits for the
  exit, so no wait needs waking and no second object has to be watched.
- **Park a slot thread on the capacity notification**: rejected because a thread parked on capacity
  cannot observe its process exiting, and waiting on the process and a wake object together would
  need a process file descriptor on Linux and a second object on both platforms; a relay thread
  resubmits parked observations while the slot thread keeps its plain exit wait.
- **One daemon-wide capacity signal for every waiter**: rejected because the supervisor drains that
  signal (ADR-0012 Section 4) and another waiter could miss it; one flag per waiter costs one bit
  each.
- **A current cell and a next-launch cell, with promotion between them**: rejected because a Job
  that moves between cells can be missed by a stop that scans the two cells while the move happens,
  and because a next-launch cell that opens only after the last submission has returned leaves a
  window, while the thread is descheduled with the submission in flight, in which the writer retires
  the Job and the next occupant finds no place; two stable cells with the `closing` phase set before
  the submission close both.
- **Retry `normal_full` with a timed backoff**: rejected because the core already offers the
  level-triggered capacity notification that ADR-0012 Section 4 defines, the Session retainer
  placeholder waits on it, and a backoff would add a duration and a poll that the notification makes
  unnecessary.
- **Let the port object hold the ingress handle**: rejected because ADR-0005 lets a port retain "its
  own adapter state but never a writer, ingress, ... pointer or reference"; the handle belongs to
  the producer.
- **Pass the verified registry entries to the runner as registry types**: rejected because one
  adapter would then include another adapter's public header, and the composition already converts
  the entries for the Backend.
- **A per-Job working directory now**: rejected because the registry file has no field for it
  (ADR-0015 Section 1) and Phase 5 decides where a Worker writes.

## References

- Issue #126; Gate #123 (P2-3, P2-7, P2-9); Issues #127, #128, #133, #107, #73, #35
- Requirements: `WRK-001` (Planned)
- Contract Registry: `Application runner and local process lifecycle`
- Related ADR: ADR-0002, ADR-0003, ADR-0004, ADR-0005, ADR-0006, ADR-0010, ADR-0012, ADR-0014,
  ADR-0015
- `spike/README.md`: findings 5, 9, and 10; "Intentionally missing"
