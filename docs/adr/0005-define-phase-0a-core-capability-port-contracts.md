# ADR-0005: Define Phase 0A core capability port contracts

## Status

Accepted — 2026-08-03

## Context

Accepted ADR-0002 defines the pure Job reducer, the complete logical JobJournal envelope, identity
bindings, timer candidates, and ordered post-sync effect identifiers. Accepted ADR-0003 defines the
single writer, logical-commit ordering, precommit materialization, no-fail postcommit ownership
handoff, ingress, callback lifetime, and shutdown. Accepted ADR-0004 requires dependency-minimal
core code and Sitometron-owned public types.

Issue #11 must add deterministic fakes before Issue #12 composes the first lifecycle. Those fakes
need narrow C++ seams for time, logical Journal commit, Application actions, Session retention, and
identity generation. The seams are shared by Issue #11, Issue #12, and later adapters, so placing
them under `include/sitometron/core` without a design authority would accidentally make
implementation choices into public contracts.

This ADR owns only that minimal capability surface and the passive fake behavior used to verify it.
It does not add a production adapter, effect executor, timer scheduler, callback API, response or ACK
port, or another lifecycle state machine.

## Decision

We will expose five capability-specific interfaces and their Sitometron-owned DTOs in the future
`include/sitometron/core/job_ports.hpp` header:

```text
ClockPort
JobJournalPort
ApplicationRunnerPort
SessionRetainerPort
IdentitySourcePort
```

The port header may include only the already reviewed standard headers `<cstdint>`, `<string>`, and
`<variant>`, plus Sitometron core headers. It exposes no dependency-owned or concurrency-owned type.
The interfaces are an internal v0.1 public C++ surface with no ABI-stability promise, consistent with
ADR-0004.

### Owned public values and semantic interface

The following pseudocode fixes the semantic shape and names that Issue #11 will implement. It is not
an implementation in this ADR-only work.

```cpp
namespace sitometron::core {

struct DiagnosticTimestamp {
  std::string rfc3339;
};

struct MonotonicInstant {
  std::uint64_t nanoseconds_since_origin = 0;
};

struct ClockReading {
  DiagnosticTimestamp recorded_at;
  MonotonicInstant monotonic_time;
};

struct LogicalJobEvent {
  std::uint32_t schema_version = 1;
  std::uint64_t sequence = 0;
  EventType event_type = EventType::kInvalid;
  DiagnosticTimestamp recorded_at;
  Uuid job_id;
  EventPayload payload;
};

enum class LogicalCommitResult {
  kCommitted,
  kDefiniteFailure,
  kOutcomeUnknown
};

struct ApplicationLaunchRequest {
  Uuid job_id;
  WorkerLaunchIntentPayload intent;
};

struct ApplicationStopRequest {
  Uuid job_id;
  StableId launch_operation_id;
  Uuid worker_id;
};

struct SessionRetainRequest {
  Uuid job_id;
  Uuid session_id;
};

struct IdentitySourceExhausted {};
struct GeneratedJobSessionIdentity { Uuid value; };
struct GeneratedWorkerIdentity { Uuid value; };
struct GeneratedLaunchOperationIdentity { StableId value; };

using JobSessionIdentityResult =
    std::variant<GeneratedJobSessionIdentity, IdentitySourceExhausted>;
using WorkerIdentityResult =
    std::variant<GeneratedWorkerIdentity, IdentitySourceExhausted>;
using LaunchOperationIdentityResult =
    std::variant<GeneratedLaunchOperationIdentity, IdentitySourceExhausted>;

class ClockPort {
 public:
  virtual ~ClockPort() = default;
  [[nodiscard]] virtual ClockReading Read() = 0;
};

class JobJournalPort {
 public:
  virtual ~JobJournalPort() = default;
  [[nodiscard]] virtual LogicalCommitResult Commit(
      const LogicalJobEvent& event) noexcept = 0;
};

class ApplicationRunnerPort {
 public:
  virtual ~ApplicationRunnerPort() = default;
  virtual void HandoffLaunch(ApplicationLaunchRequest&& request) noexcept = 0;
  virtual void HandoffCooperativeStop(ApplicationStopRequest&& request) noexcept = 0;
  virtual void HandoffForcedStop(ApplicationStopRequest&& request) noexcept = 0;
};

class SessionRetainerPort {
 public:
  virtual ~SessionRetainerPort() = default;
  virtual void HandoffRetainSameIdentity(SessionRetainRequest&& request) noexcept = 0;
};

class IdentitySourcePort {
 public:
  virtual ~IdentitySourcePort() = default;
  [[nodiscard]] virtual JobSessionIdentityResult GenerateJobSessionIdentity() = 0;
  [[nodiscard]] virtual WorkerIdentityResult GenerateWorkerIdentity() = 0;
  [[nodiscard]] virtual LaunchOperationIdentityResult
  GenerateLaunchOperationIdentity() = 0;
};

}  // namespace sitometron::core
```

Issue #11 may add equality operators and mechanically necessary default member values without
changing these semantics. It may not add another expected result, callback, generic context,
borrowed request field, or capability method without renewed ADR review.

### Clock boundary

`DiagnosticTimestamp` contains a schema-valid RFC 3339 `date-time` string suitable for the
ADR-0002 `recorded_at` field. It is diagnostic only. It establishes no ordering, causality, timeout,
or retry authority.

`MonotonicInstant` is a nondecreasing, non-wrapping nanosecond coordinate from an unspecified origin
owned by one Clock instance. Values are comparable only when produced by the same instance. It is
never serialized as `recorded_at`.

One `Read()` returns both values as distinct owned types. A read does not advance time. A Clock read
or value-materialization exception occurs before logical commit and follows ADR-0003's precommit
fail-closed path. This ADR adds no public Clock error catalogue.

The Issue #11 fake starts from explicitly configured values. The harness changes the diagnostic
value explicitly and advances monotonic time only through a test-support operation with checked,
non-wrapping arithmetic. The fake does not read the host clock, sleep, schedule a timer, invoke a
callback, or emit a candidate automatically. Timer phase, generation, arm/disarm, callback lease,
and candidate admission remain Issue #12 responsibilities under ADR-0002 and ADR-0003.

### Logical JobJournal boundary

`LogicalJobEvent` is the C++ projection of the six-field ADR-0002 envelope:

```text
schema_version
sequence
event_type
recorded_at
job_id
payload
```

The writer supplies a schema version of 1, a nonzero non-wrapping sequence, a schema-valid diagnostic
timestamp, and a matching event discriminator/payload. It constructs the complete value before the
Journal call. The Journal assigns or changes none of those fields.

`Commit()` is one synchronous logical-commit request. It borrows the complete immutable event only
for that call, retains no alias after return, and returns exactly one of:

- `kCommitted`: the logical commit boundary succeeded;
- `kDefiniteFailure`: the implementation knows that the event did not commit; or
- `kOutcomeUnknown`: the implementation cannot establish whether the event committed.

The port is non-throwing. Every implementation catches every internal allocation, copy, adapter, and
foreign exception before it could cross the port boundary. It maps an error to `kDefiniteFailure`
only when absence of commit is known; otherwise it returns `kOutcomeUnknown`. The `noexcept`
declaration alone is not evidence that the implementation is safe. There is no fourth expected
outcome. Neither noncommitted result authorizes retry, sequence reuse, reducer apply, effect handoff,
response release, or ACK authorization. Issue #12 burns the allocated sequence and follows
ADR-0003's failure latch and source-disposition rules.

The Phase 0A fake compares and records logical attempts and scripts all three results. It configures
finite expected attempts and observation capacity before use, performs no unbounded growth inside
the non-throwing call, and converts any internal observation failure to a sticky harness failure plus
`kOutcomeUnknown`. Only `kCommitted` is commit evidence. The fake claims no serialization, encoding,
filesystem append, flush, disk sync, crash durability, replay, or recovery; those remain Phase 0B.

### Precommit materialization and Application boundary

Issue #12 exclusively maps ordered reducer effect identifiers to fully owned capability requests.
Every potentially failing copy, validation, and allocation completes before logical commit. No port
receives an `Effect`, `Snapshot`, proposal, `string_view`, reference member, or borrowed adapter
state.

The materialization sources are fixed as follows:

- launch: committed-envelope `job_id` plus the exact accepted `WorkerLaunchIntentPayload`;
- cooperative or forced stop: the bound Job, launch-operation, and Worker identities available to
  the accepted transition;
- Session retention: committed-envelope `job_id` plus the exact accepted Session identity;
- timer operations: writer-owned timer state and lifecycle position, outside the public ports in this
  ADR;
- ACK and terminal publication: bound data from the prospective pure `ApplyResult` computed and kept
  private before commit, outside the public ports in this ADR.

`ApplicationLaunchRequest` deliberately adds only `job_id` to the exact accepted launch-intent
payload. It does not add executable paths, commands, environment variables, credentials, Session
APIs, resolved-allocation interpretation, or proprietary Application data. Stop requests contain
only the three existing bound identities. Cooperative and forced stop remain separate operations.

The rvalue-reference `noexcept` calls consume pre-materialized owned requests after commit. Entry
into a handoff is the non-throwing ownership-transfer boundary required by ADR-0003; Issue #12 must
invoke every handoff with an rvalue, and the caller does not retain an alias. Before commit, Issue #12
composition must establish the bounded destination capacity that the handoff will consume. After call
entry, the implementation may only move into that already prepared storage and must catch no
exception because it performs no potentially throwing copy, allocation, or synchronous external
operation. The `noexcept` declaration alone is not evidence that these conditions hold. Issue #11
test/support code must assert that `ApplicationLaunchRequest`, `ApplicationStopRequest`, and
`SessionRetainRequest` are nothrow move constructible; this test-only assertion does not authorize
`<type_traits>` or expand the production-header allowlist. The port performs no synchronous
operational result, callback, ingress submission, retry, deduplication, once-only decision, or
Worker stop-and-wait logic. Launch observations and process-exit outcomes return later as
independently admitted raw candidates.

### Session-retention boundary

`SessionRetainRequest` contains the routing Job ID and bound Session ID. In v0.1 the two UUIDv7 values
must be equal under ADR-0002. `HandoffRetainSameIdentity()` is the same rvalue-reference,
non-throwing, postcommit ownership transfer as the Application handoffs and returns no synchronous
result.

The only candidate an Issue #11 Session fake may stage is matching `session_retained`.
`session_retain_requested`, `finalization_completed`, and `finalization_failed` remain explicit
harness inputs. This ADR defines no Sitos API, production retention policy, or generic finalization
port.

### Identity-source boundary

The three generation operations are capability-specific even though they share one narrow source:

- generate one valid UUIDv7 that is reused exactly as Job and Session identity;
- generate one valid UUIDv4 Worker identity; and
- generate one valid ADR-0002 `StableId` launch-operation identity.

A call returns either a generated owned value or `IdentitySourceExhausted`. A source instance must
not issue the same generated value twice. Invalid or duplicate scripted values are fake-setup errors,
not another runtime result. Exhaustion is the only expected generation failure result and is a
precommit materialization failure handled by Issue #12. Generation is not a postcommit handoff and
is intentionally not `noexcept`: an unexpected allocation, formatting, entropy-adapter, or foreign
exception also stays on ADR-0003's precommit fail-closed path. It does not create another public
result category.

The Issue #11 fake consumes finite pre-scripted values and exhaustion markers. It does not read a
Clock, randomness source, environment, or host state and does not call Boost generators or a CSPRNG.
ADR-0004 continues to defer production UUID clock, randomness, rollback, same-millisecond, and
security behavior to the later owning design.

### Passive deterministic fakes and candidates

Issue #11 fakes use finite scripts configured before capability calls. Each fake owns its
observations; it does not share a mutable service context with another fake. Unexpected or duplicate
calls, invalid setup, capacity overflow, or unused mandatory expectations set a per-fake sticky
harness failure that an explicit verification operation reports. Because ownership already
transferred at a runner or Session handoff, a handoff mismatch consumes and destroys that request,
records no successful observation, and stages no candidate. Setup or capacity failure must normally
be detected before any commit; observing it after commit makes the test fail rather than inventing a
runtime recovery path.

Runner launch may stage only a `worker_launch_observed{started|failed}` or
`process_exit_confirmed{completion_mode=process_already_exited}` raw candidate carrying the exact Job
and launch-operation identities representable by that candidate; the started/failed outcome is
independently scripted. Cooperative-stop handoff may stage only matching
`process_exit_confirmed{completion_mode=cooperative}`, and forced-stop handoff may stage only matching
`process_exit_confirmed{completion_mode=forced}`. `worker_running`, `worker_completed`, and
`worker_failed` remain harness-owned inputs. A Session handoff may stage only `session_retained` with
the exact Job and Session identities.

Every staged candidate is inert and test-owned. Each fake retains candidates in script order. An
explicit harness take operation transfers and removes the oldest owned `RawCandidateEvent`; taking
from an empty fake reports no candidate and fabricates nothing. Explicit cancellation destroys the
selected pending candidate. A mandatory candidate that is neither taken nor explicitly cancelled
makes verification fail. These ownership and ordering rules are contractual, but the helper class
and method spelling remain Issue #11 implementation details.

No fake stores or invokes a callback, lambda, ingress handle, writer, reducer, snapshot, response
adapter, ACK transport, raw pointer, reference, or view. No fake assigns ingress sequence, Journal
sequence, or `recorded_at`.

A separate passive test-only observer may record explicitly supplied effect identifiers, responses,
and ACK observations with local ordinal values. Its finite capacity is configured before any
postcommit observation. Overflow sets sticky harness failure and records no false observation. It
executes, releases, authorizes, or submits nothing and is not a production port. Issue #12 explicitly
feeds it while proving cross-capability ordering; Issue #11 only verifies its inert recording
contract.

### Port-object ownership and reentrancy

Issue #12 composition owns every port object, fixes each binding before writer startup, and keeps the
objects alive and never rebound until the writer is stopped and no port call can remain in flight. A port
may retain its own adapter state but never a writer, ingress, reducer, snapshot, response, ACK, or
effect-dispatch pointer or reference.

No Clock, Journal, runner, Session, identity, or fake call may synchronously invoke or re-enter writer,
ingress, reducer, response, ACK, or effect-dispatch code. Later operational facts use the independently
owned ingress path defined by ADR-0003; the Issue #11 fakes only return inert owned candidates to the
test harness.

### Explicitly excluded public mechanisms

This ADR does not add:

- an EffectExecutor port or payload-bearing effect variant;
- a Timer port, scheduler, duration policy, timer callback, or automatic candidate source;
- a response, ACK, readiness, resource, finalization, or generic callback port;
- a future, promise, `std::function`, service locator, or mutable context bag; or
- an ingress, queue, thread, mutex, callback lease, retry, or Worker-delivery API.

ADR-0003 and Issue #12 already own effect dispatch and private orchestration. The current reducer
continues to emit ordered `EffectId` values without adapter arguments; this ADR does not change that
contract.

### Requirements and stable support checks

No new Requirement ID is necessary. This ADR introduces enabling C++ contracts and deterministic
support, not new product behavior.

- Directly applicable: `NFR-004`, `NFR-005`.
- Build-platform evidence: `NFR-001`, `NFR-003`.
- Logical-surface constraints: `JRN-001` through `JRN-003`.
- Not implemented by this ADR or Issue #11: `JOB-004` through `JOB-008` and `OPS-001`.

This ADR registers these names as Planned; Issue #11 later adds the CTest targets:

| Stable check | Requirement mapping and limit |
|---|---|
| `core_port_fake_contracts` | `NFR-004`; verifies Clock separation, identity exhaustion, owned handoffs, inert candidates, and expectation failures |
| `job_fake_logical_commit_results` | Support for `JRN-001` and `JRN-003`; verifies complete-envelope pass-through and all three logical results, but not writer reaction or physical durability |
| `job_fake_effect_observation` | `NFR-004` and support for `JRN-002`; verifies passive ordered observation only, not commit-before-effect orchestration |

The existing Issue #12 checks remain authoritative for `JOB-008`, `OPS-001`,
`job_logical_commit_order`, and `job_logical_commit_failure_fail_closed`. The three support checks do
not claim implementation of lifecycle, ingress, shutdown, logical ordering, or physical durability.

### Dependency boundary

ADR-0005 adds no standard-header authorization, third-party dependency, manifest port, direct CMake
target, or link target. In particular, it does not authorize `<chrono>`, `<functional>`,
concurrency headers, dependency-owned public types, or `Threads::Threads` for Issue #11. The existing
`NFR-005` checks must scan the future public header and remain passing.

ADR-0003's four private concurrency headers and private `Threads::Threads` linkage remain reserved
for the later Issue #12 implementation and may not appear in this public port surface.

### Contract Registry and implementation ownership

With this ADR Accepted, the Registry records:

```text
Core lifecycle capability ports | Normative | Planned | Accepted ADR-0005 under Issue #26 | Phase 0A
```

The affected existing rows do not transition in this ADR-only acceptance publication:

- `Approved core dependency boundary and allowlist`: `Normative | Implemented`;
- `JobJournal envelope and event schemas`: `Normative | Planned`; and
- `Single-state-writer ingress and critical reserve`: `Normative | Planned`.

The owner accepted the exact reviewed draft head
`04e42ec180353790708cc3a74f70ef5109d1fdaa` in the
[PR #27 acceptance record](https://github.com/tetsuh/sitometron/pull/27#issuecomment-5172327903).
This publication changes ADR-0005 from Proposed to Accepted and the new row from
`Planned | Planned` to `Normative | Planned`. Merge authorization is a separate owner decision.
Acceptance alone does not implement a port.

After this ADR merges, Issue #11 may implement the new row and transition it to
`Normative | Implemented`. Issue #11 may transition the JobJournal envelope implementation to
`Normative | In progress` after the owned C++ envelope and logical fake exist. Issue #12 alone owns
the complete logical-envelope `Normative | Implemented` transition after writer construction,
sequence allocation, and logical ordering are proved. Physical Journal durability remains Phase 0B.

## Consequences

- Good: Issue #11 and Issue #12 share one reviewed, capability-specific C++ boundary instead of
  freezing ad hoc fake APIs.
- Good: Journal commit outcomes, sequence ownership, and postcommit handoff failure semantics cannot
  be conflated.
- Good: Fakes remain deterministic, passive, and independent of threads, callbacks, real time, I/O,
  and production adapters.
- Good: Public types remain Sitometron-owned and fit the implemented dependency allowlist.
- Bad: Five abstract interfaces and several owned DTOs are introduced before a production adapter
  exists.
- Bad: Non-throwing postcommit handoff requires Issue #11 fakes and later adapters to prepare bounded
  ownership capacity before a call.
- Bad: Production Clock, identity, process, and Session behavior will require later design without
  changing these Phase 0A ownership rules.
- Neutral: `sitometron_core` has no ABI-stability promise in v0.1, but semantic changes still require
  contract review.
- Neutral: This ADR accepts the port contracts but does not implement them; its Accepted status is
  contract authority, not implementation evidence.

## Options considered

- **Keep the ports private to Issue #12**: rejected because Issue #11 fakes, Issue #12 composition,
  and later adapters share the boundary.
- **Use one generic service context**: rejected because it hides capability dependencies and creates
  mutable cross-service ownership.
- **Add an EffectExecutor port or payload-bearing effect variant**: rejected because ADR-0003 and
  Issue #12 own mapping and dispatch, while ADR-0002 intentionally emits ordered identifiers.
- **Add a Timer port**: rejected because query-only time is sufficient for Issue #11 and timer
  generation, scheduling, callback lifetime, and admission belong to Issue #12.
- **Use asynchronous callbacks, futures, or promises**: rejected because they would import ingress
  lifetime, synchronization, and retry semantics into Issue #11.
- **Borrow runner or Session request data**: rejected because copying after commit could fail and
  borrowed lifetime would cross the non-throwing handoff boundary.
- **Expose `std::chrono` or dependency-owned values**: rejected because Sitometron-owned scalar and
  string types keep the public boundary closed and mechanically reviewable.
- **Add more Journal result categories**: rejected because definite failure and outcome unknown are
  the only distinct fail-closed authorities required by ADR-0003.
- **Implement production adapters with the fakes**: rejected because process control, Sitos,
  physical Journal durability, timer scheduling, and production identity generation have later
  owners.

## References

- Issue #26
- Gate #1
- Requirements: `NFR-001`, `NFR-003`, `NFR-004`, `NFR-005`, `JRN-001` through `JRN-003`;
  affected but not implemented: `JOB-004` through `JOB-008`, `OPS-001`
- Contract Registry: new `Core lifecycle capability ports`; affected `Approved core dependency
  boundary and allowlist`, `JobJournal envelope and event schemas`, and `Single-state-writer ingress
  and critical reserve`
- Related ADRs: [ADR-0002](0002-define-core-job-reducer-contract.md),
  [ADR-0003](0003-define-single-state-writer-ingress-contract.md), and
  [ADR-0004](0004-allow-explicit-core-dependencies.md)
