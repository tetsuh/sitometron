# Core contracts

Accepted ADR-0002 makes the Phase 0A core Job contract normative. Implementation remains Planned.

## 1. Source files

The Phase 0A Job reducer contract is split into reviewable machine-readable artifacts:

- [`job-command.schema.json`](../schemas/core/v1/job-command.schema.json): strict core cancel and
  terminate command DTO;
- [`job-candidate-event.schema.json`](../schemas/core/v1/job-candidate-event.schema.json): strict raw
  lifecycle-candidate DTO that excludes command-owned and reducer-normalized event kinds;
- [`job-reducer-input-event.schema.json`](../schemas/core/v1/job-reducer-input-event.schema.json):
  complete internal pre-envelope event DTO passed to the reducer matrix;
- [`job-rejection.schema.json`](../schemas/core/v1/job-rejection.schema.json): closed stable rejection
  reasons;
- [`job-reducer-snapshot.schema.json`](../schemas/core/v1/job-reducer-snapshot.schema.json): closed
  reducer state and durable identity bindings;
- [`job-journal-event.schema.json`](../schemas/core/v1/job-journal-event.schema.json): strict logical
  JobJournal envelope and typed event payloads;
- [`job-reducer-contract.schema.json`](../schemas/core/v1/job-reducer-contract.schema.json): JSON
  Schema for the contract document;
- [`job-reducer-contract.json`](../schemas/core/v1/job-reducer-contract.json): closed enums,
  command decisions, and the exhaustive state/event matrix;
- [`job-reducer-vectors.schema.json`](../schemas/core/v1/job-reducer-vectors.schema.json): JSON Schema
  for the fixture document;
- [`job-reducer-vectors.json`](../schemas/core/v1/job-reducer-vectors.json): one concrete reachable
  fixture per ordered matrix case, concrete invalid-payload fixtures, and chained sequence/race
  fixtures;
- [`job-state-diagram.dot`](../schemas/core/v1/job-state-diagram.dot): human-readable Graphviz state
  diagram whose edge inventory is checked against the matrix;
- [`validate_core_contract.py`](../tools/validate_core_contract.py): pinned development-time Draft
  2020-12 metaschema, local-reference, format, and instance validation;
- [`check_core_job_contract.cmake`](../cmake/check_core_job_contract.cmake): dependency-free
  Sitometron-specific structural, cross-field, coverage, fixture, and diagram consistency check.

ADR-0002 explains the decision and authority boundary. The JSON contract defines exact cases.
Physical JobJournal durability remains Phase 0B scope.

## 2. Lifecycle model

The public closed state set is:

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

`absent` appears only as an entity position for `job_created`. It is not a Job state.

The state enum does not contain enough information to decide every input. The reducer snapshot also
tracks the first latched reason, successful completion candidate, completion mode, resource status,
allocation identity and digest, Worker-launch operation and Worker identity, process-exit
confirmation, Session identity and retention status, finalization status, cleanup status, and a
terminal Worker-event acknowledgment obligation. The same Controller-issued UUIDv7 is both the Job
ID and Session ID in v0.1; a schema annotation and the contract-specific checker enforce equality.

## 3. Decision and apply boundary

The single state writer evaluates commands and internal reducer-input events against one snapshot.
Raw lifecycle candidates exclude `cancel_accepted`, `terminate_accepted`, and `late_worker_event`;
only the command table or reducer normalization may create those internal event kinds.

```text
input
  -> pure decision
      -> reject(reason), with no Journal record
      -> accepted Journal event
          -> append and disk sync
          -> pure reducer apply
          -> typed post-sync effects
```

The four exhaustive dispositions are `transition`, `audit`, `late_audit`, and `reject`. A reject case
cannot emit, update, request an effect, or normalize to another event kind. Cases use a closed
predicate AST and ordered-first-match semantics, with a mechanically checked final `otherwise` for
conditional rows. An effect identifier is a request to an adapter or supervisor; the reducer never
performs I/O.

`cancel` and `terminate` use the command matrix because rejection occurs before the accepted-control
Journal event exists. Lifecycle facts use the event matrix. A delayed terminal Worker fact is
normalized to `late_worker_event` before append; an already normalized late fact is copied without
wrapping it again.

Timer generation is decided before event-matrix ingress. Each timeout phase has an independent
active generation. A notification matching its phase's active generation emits a `timeout_expired`
candidate; stale or disarmed notifications emit no candidate and no Journal event. Attempting to arm
after the non-wrapping uint64 generation is exhausted fails closed. Four dedicated fixtures are
evaluated against the machine-readable timer contract.

## 4. Completion and cleanup axes

`worker_completed` creates a successful completion candidate and enters `finalizing`. Success is not
committed until finalization has completed and `terminal_outcome_committed{outcome=succeeded}` has
been synced.

Preparation timeout enters `finalizing` directly when no process may exist and enters `stopping`
with cooperative stop otherwise. The execution timer remains active through terminal commit. Its
expiration in `stopping` forces stop without replacing the latched reason; expiration in `finalizing`
may replace a sole success candidate with `timed_out`. An earlier cancel, terminate, timeout, or
failure reason remains immutable.

A terminal Worker event binds an ACK obligation to Worker ID and event sequence. Terminal commit
enables an idempotent Phase 2 ACK retry, and first confirmed process exit clears the binding. Normal
success may become terminal before process exit. A later confirmed exit must match the launch
operation binding and sets completion mode. Resource release must match the committed allocation ID
and digest, and cleanup facts are monotonic state-preserving terminal audits. A
`process_exit_confirmation` timeout may force stop and quarantine resources without changing the
terminal result.

## 5. Contract checks

Run the generic Draft 2020-12 validation and the contract-specific check directly:

```sh
uv sync --frozen --only-group schema
uv run --frozen --only-group schema python tools/validate_core_contract.py
cmake \
  -DSITOMETRON_SOURCE_DIR="$PWD" \
  -P cmake/check_core_job_contract.cmake
```

The generic validator checks all eight core schemas and the contract and vector documents. It does
not interpret Sitometron extension annotations. The dependency-free CMake check owns exact digest,
UTF-8 byte-limit, cross-field identity, matrix reachability, timer, sequence, and DOT consistency.
The configured test suite exposes the latter as CTest `core_job_contract`; Linux and Windows CI run
both checks.
