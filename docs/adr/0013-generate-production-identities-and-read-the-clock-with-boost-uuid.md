# ADR-0013: Generate production identities and read the clock with Boost.UUID

## Status

Proposed — 2026-10-25

## Context

ADR-0005 defines two capability ports that every `sitometrond` composition must supply:
`IdentitySourcePort`, with three generation operations (one UUIDv7 reused as Job and Session
identity, one UUIDv4 Worker identity, one `StableId` launch-operation identity), and `ClockPort`,
whose `Read()` returns a diagnostic RFC 3339 `recorded_at` and a per-instance `MonotonicInstant`.
ADR-0002 fixes the UUID versions and the `StableId` grammar through the Journal event schema
(`^[A-Za-z0-9][A-Za-z0-9._:-]*$`, 1 to 128 characters). ADR-0004 selects Boost.UUID for values,
parsing, and formatting, authorizes `<boost/uuid/time_generator_v7.hpp>` and
`<boost/uuid/random_generator.hpp>` "only behind the later owning generator adapter", and defers the
UUIDv7 clock, random-source, rollback, monotonicity, and same-millisecond rules to that adapter's
Issue. Issue #102 is that Issue.

Today only the walking skeleton implements the two ports (`spike/src/system_ports.cpp`), with a
hand-written UUIDv7 and a `launch-N` counter. The production composition of Issue #105 and #106
supplies placeholders that are never reached: the identity source returns `IdentitySourceExhausted`
and the clock throws, and the contract tests prove zero calls. Phase 2 registration needs real
implementations.

The code fixes three facts the design depends on:

- `JobOrchestrator::CreateJob` is the only caller of the three generation operations. It calls them
  on the creating thread, outside the ingress mutex, after setting `creation_generation_in_progress`
  so that at most one creation generates at a time (`src/core/job_orchestrator.cpp`, around line
  1750). A thrown exception or an `IdentitySourceExhausted` result latches the writer's failure
  state, as ADR-0005 says of precommit materialization.
- `ClockPort::Read()` has one caller, on the writer thread, immediately before the Journal append of
  each accepted turn (around line 1414).
- No Sitometron code orders Jobs, Sessions, or launches by the text of their identifiers. The
  resident list is ordered by Journal sequence (ADR-0008 Section 4) and the Phase 1 index is seeded
  from replay order (Issue #106).

Boost.UUID 1.91.0, the pinned baseline, implements the generators as follows (read from
`include/boost/uuid/time_generator_v7.hpp` and `random_generator.hpp` at tag `boost-1.91.0`):

- `time_generator_v7::operator()` is `noexcept`. It reads `std::chrono::system_clock` in
  microseconds and keeps a 64-bit state of the millisecond, the microsecond within it, and a 6-bit
  counter. The millisecond fills `unix_ts_ms`; the microsecond and the counter fill `rand_a` and the
  top of `rand_b` (RFC 9562 Section 6.2, method 3 with a counter). When the clock has advanced, the
  counter resets; when the same microsecond repeats, the counter increments and carries into the
  microsecond field, so values strictly increase within one instance while the clock does not go
  backwards. When the millisecond goes backwards, the generator uses the new, earlier timestamp and
  is not monotonic across that step. The remaining 58 random bits come from a ChaCha20 stream seeded
  from the operating system's entropy provider.
- `random_generator` is the same ChaCha20 stream seeded from the operating system's entropy
  provider; it produces UUIDv4 values and its call operator does not fail.
- The only operation that can fail is seeding the entropy provider, which happens when a generator
  is constructed.

The owner decided on 2026-10-25 under Issue #102:

- the production source and clock live in a dedicated adapter target, not in the composition root
  and not in `sitometron_core`; and
- the UUID generators are Boost.UUID's, used as they are; Sitometron does not assemble UUIDs itself.

## Decision

We will implement the production `IdentitySourcePort` and `ClockPort` in one small adapter target,
`sitometron_identity`, built on Boost.UUID's `time_generator_v7` and `random_generator` and on the
standard `<chrono>` clocks, with the rules below.

### 1. UUIDv7 for Job and Session identity

The source generates the Job and Session identity with one `boost::uuids::time_generator_v7`
instance owned by the source for the process lifetime, and formats it as the canonical lowercase
`8-4-4-4-12` text of ADR-0004.

- Timestamp source: `std::chrono::system_clock`, as the generator reads it. The time zone is not
  involved; the value is milliseconds since the Unix epoch.
- Random source: the generator's ChaCha20 stream, seeded once from the operating system's entropy
  provider when the source is constructed.
- Ordering within one millisecond: Boost's method, as described in the Context. Within one source
  instance, successive values strictly increase while the system clock does not go backwards. No
  Sitometron component relies on this ordering; it is a property, not a contract that other code may
  depend on.
- Clock moving backwards: the generator takes the new, earlier timestamp. The source does not detect
  or correct this. A value generated after the step can sort before a value generated before it.
  This is acceptable because nothing orders by identifier text, and uniqueness does not depend on
  monotonicity (Section 2).
- `IdentitySourceExhausted`: never returned by the production source. The generator cannot fail per
  call. The source constructs its generators when the composition starts; if seeding the entropy
  provider throws, startup fails with the Issue #105 startup-refusal status and no listener opens.

### 2. Uniqueness across restarts

New identities rely on the millisecond timestamp and the random bits alone. The source does not read
the replayed state and does not refuse a value that an earlier run issued. Reasons:

- ADR-0005 makes the source a passive capability that the writer calls; a source that consulted
  resident snapshots would invert that direction and couple it to the Journal.
- A collision needs the same millisecond and the same 74 random bits as a replayed Job, with
  probability far below any operational concern.
- The core already refuses the collision: `job_created` for an existing resident is rejected as
  `job_already_exists`, the adapter maps that to `500` `service`/`internal` (Issue #106), and no
  Journal record is written. The next create generates a new value.

Within one process, "a source never issues the same value twice" (ADR-0005) holds by construction:
the UUIDv7 generator strictly increases while the clock does not go backwards, and after a backwards
step the random bits make a repeat as unlikely as a cross-restart collision. The UUIDv4 and
`StableId` values are random.

### 3. UUIDv4 for Worker identity

The source generates the Worker identity with one `boost::uuids::random_generator` instance owned
for the process lifetime, formatted as canonical text. The value identifies a Worker and nothing
else; ADR-0004's rule stands that a UUID is never a credential, and Issue #15 keeps the Worker bearer
token a separate 256-bit secret.

### 4. `StableId` for launch operations

The launch-operation identity is the canonical text of a UUIDv7 drawn from the same
`time_generator_v7` instance as Section 1: 36 characters of lowercase hexadecimal digits and
hyphens, which satisfies the ADR-0002 grammar `^[A-Za-z0-9][A-Za-z0-9._:-]*$` with 1 to 128
characters. It is unique by the same argument as Section 2 and needs no state, unlike the skeleton's
`launch-N` counter, which restarts at 1 after every restart and would collide with replayed launch
operations.

### 5. Clock

`ClockPort::Read()` returns:

- `recorded_at`: `std::chrono::system_clock::now()` formatted as RFC 3339 `date-time` in UTC with
  millisecond precision and the `Z` designator, for example `2026-10-25T04:05:06.789Z`. It is
  diagnostic only (ADR-0005); it establishes no ordering and is never compared.
- `MonotonicInstant`: the nanoseconds of `std::chrono::steady_clock::now()` since the clock
  instance's origin, which is the instant the clock was constructed, at composition start. The value
  is nondecreasing and non-wrapping within the process. Across restarts it restarts at zero and is
  never compared with a value from another process, as ADR-0005 requires.

Both readings come from one `Read()` call and do not advance time.

### 6. Threading

The core calls the three generation operations from the creating thread, outside the ingress mutex,
and at most one creation generates at a time (`creation_generation_in_progress`). The core reads the
clock on the writer thread only. The source and the clock therefore need no internal locking, and
this ADR specifies none. The adapter states this precondition in its header comment; a future caller
that violates it changes this ADR.

### 7. Placement and dependencies

The source and the clock live in `sitometron_identity`, a static library under `src/identity/` with
Sitometron-owned public headers under `include/sitometron/identity/`. `sitometron_core` is not
changed. The composition root links `sitometron_identity` and passes the two port objects to the
core, in both the HTTP-enabled and the HTTP-off builds.

Only these manifest ports, direct CMake targets, and headers are authorized for
`sitometron_identity`, in the format of ADR-0010 Section 1:

| Manifest port | Pinned-baseline version | Direct CMake target | Authorized headers |
|---|---:|---|---|
| `boost-uuid` | 1.91.0 | `Boost::uuid` | `<boost/uuid/uuid.hpp>`, `<boost/uuid/uuid_io.hpp>`, `<boost/uuid/time_generator_v7.hpp>`, and `<boost/uuid/random_generator.hpp>` |

The list restricts direct includes; headers of `boost-uuid` that a listed header includes are
permitted as its implementation. The target adds no manifest port. Its public headers expose only
Sitometron-owned types, and it links `Boost::uuid` privately. The implementation Issue adds the
dependency-allowlist and public-API-isolation scanners for this target in the pattern of the
`sitometron_http` scanners, and keeps the `NFR-005` checks that reject the generator headers in the
core.

### 8. Not decided here

- Worker bearer credentials (Issue #15; ADR-0004).
- Timer durations and the timer port (H11), which reads no identity and is a separate design.
- Resolving Jobs that replay leaves unresolved (ADR-0006; #35, #73).
- Any ordering of Jobs, Sessions, or launches by identifier text; none exists and none is added.

## Requirements and checks

This ADR adds `ID-001` (MUST): generate every production Job, Session, Worker, and launch-operation
identity with the source of this ADR, and read `recorded_at` and `MonotonicInstant` with its clock,
so that each value is canonical, each identifier is unique within the process by construction, and
no production path reads another clock or entropy source for identity.

Its named checks, Planned until the implementation Issue activates them:

- `identity_uuidv7_canonical`: ten thousand Job and Session identities and launch identities from
  one source are canonical UUIDv7 text with the RFC 9562 version and variant bits, and all distinct;
- `identity_uuidv7_monotonic_burst`: successive Job identities from one source, generated without
  sleeping, strictly increase as 128-bit values while the system clock is not moved;
- `identity_uuidv4_canonical`: ten thousand Worker identities are canonical UUIDv4 text with the
  version and variant bits, and all distinct;
- `identity_stable_id_grammar`: every launch-operation identity matches the ADR-0002 `StableId`
  grammar and length;
- `identity_source_never_exhausted`: the production source returns a generated value for every call
  and never `IdentitySourceExhausted`;
- `clock_recorded_at_rfc3339`: `recorded_at` formatted from an injected `system_clock::time_point`
  is the expected UTC RFC 3339 text with millisecond precision, including the epoch, a leap day, and
  the last millisecond of a year;
- `clock_monotonic_instant`: `MonotonicInstant` computed from injected `steady_clock` readings is
  nondecreasing, starts at zero at the origin, and uses checked arithmetic;
- `identity_dependency_allowlist` and `identity_public_api_isolation`: the `sitometron_identity`
  target includes only the headers of Section 7 and exposes only Sitometron-owned types; and
- `sitometrond_composes_production_identity`: the production composition constructs the real source
  and clock, in both the HTTP-enabled and HTTP-off builds, and a create on a registered test
  Application in the contract fixture commits with a canonical UUIDv7 `job_id`.

The formatting and arithmetic checks inject their time points and run no real sleeps. The generator
checks observe Boost's generators as black boxes; they do not move the system clock, so the
clock-rollback behavior of Section 1 is stated from the Boost 1.91.0 source and not tested.

Traceability from Issue #102's required decisions:

| Required decision | Section | Checks |
|---|---|---|
| 1. UUIDv7: timestamp, randomness, same-millisecond method, rollback, exhaustion | 1 | `identity_uuidv7_canonical`, `identity_uuidv7_monotonic_burst`, `identity_source_never_exhausted` |
| 2. Uniqueness across restarts | 2 | `identity_uuidv7_canonical` (distinctness); the core's `job_already_exists` refusal, tested by Issue #106 |
| 3. UUIDv4 Worker identity | 3 | `identity_uuidv4_canonical` |
| 4. `StableId` launch identity | 4 | `identity_stable_id_grammar` |
| 5. Clock | 5 | `clock_recorded_at_rfc3339`, `clock_monotonic_instant` |
| 6. Threading | 6 | stated precondition; no check, because the core already serializes the calls |
| 7. Placement and dependencies | 7 | `identity_dependency_allowlist`, `identity_public_api_isolation`, `sitometrond_composes_production_identity` |
| 8. Tests | this section | all of the above |

## Consequences

- Good: the production daemon can create Jobs with real identities as soon as Phase 2 registers an
  Application, with no change to the core or its ports.
- Good: Sitometron implements no UUID, random, or clock algorithm; the generators are Boost's, as
  ADR-0004 chose, and the adapter is a thin wrapper that tests can observe.
- Good: the generator headers stay confined to one target that a scanner guards, as ADR-0004
  requires.
- Bad: the UUIDv7 values are not monotonic across a backwards step of the system clock. Nothing
  depends on that order, and this ADR forbids adding such a dependency without changing it.
- Bad: the generator checks are property checks, not exact-value checks, because the generators are
  not seeded from a test seam.
- Neutral: `sitometron_identity` is a third adapter target with its own allowlist and scanners.

## Options considered

- **Implement UUIDv7 in Sitometron with injected time and randomness**: rejected because ADR-0004
  rejected implementing UUID generation in Sitometron, and the only gain is exact-value tests of
  code that would then be Sitometron's to maintain.
- **Make the source refuse identifiers already present in the replayed state**: rejected because it
  inverts the ADR-0005 port direction, couples the source to the Journal, and guards against a
  collision the reducer already refuses.
- **Keep the skeleton's `launch-N` counter for `StableId`**: rejected because it restarts after
  every restart and collides with replayed launch operations.
- **Detect a backwards system clock and refuse or wait**: rejected because nothing orders by
  identifier text; waiting would block the creating thread for an unbounded time, and refusing would
  turn a clock adjustment into a service failure.
- **Place the source and clock in `sitometrond_composition`**: rejected because that target exists
  only in the HTTP-enabled build, so the identity and clock checks would not run in the HTTP-off
  build, and no scanner guards its dependencies.
- **Use `std::random_device` directly for UUIDv4**: rejected because Boost's `random_generator`
  already seeds from the operating system and is the generator ADR-0004 authorized.

## References

- Issue #102
- Gate #101 (P1-10)
- Requirements: `ID-001` (Planned)
- Contract Registry: `Production identity source and clock`
- Related ADR: ADR-0002, ADR-0004, ADR-0005, ADR-0006, ADR-0010
- RFC 9562, Sections 5.4, 5.7, and 6.2; RFC 3339
- Boost.UUID 1.91.0, `time_generator_v7.hpp` and `random_generator.hpp`
