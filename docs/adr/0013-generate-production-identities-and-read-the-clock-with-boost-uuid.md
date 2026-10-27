# ADR-0013: Generate production identities and read the clock with Boost.UUID

## Status

Accepted — 2026-10-28

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

- `JobOrchestrator::CreateWith` is the only caller of the three generation operations. It calls them
  on the creating thread, outside the ingress mutex, after setting `creation_generation_in_progress`
  so that at most one creation generates at a time (`src/core/job_orchestrator.cpp`, around line
  1750). A thrown exception or an `IdentitySourceExhausted` result latches the writer's failure
  state, as ADR-0005 says of precommit materialization.
- `ClockPort::Read()` has one caller, on the writer thread, during the precommit materialization
  of an accepted turn (around line 1414): after the reducer accepted the input and before the
  effect, source-capacity, and trace preparation that precede the Journal append. A preparation
  failure after the read fails the turn without an append, so a reading is not always followed by a
  record.
- No Sitometron code orders Jobs, Sessions, or launches by the text of their identifiers. The
  resident list is ordered by Journal sequence (ADR-0008 Section 4) and the Phase 1 index is seeded
  from replay order (Issue #106).

Boost.UUID 1.91.0, the pinned baseline, implements the generators as follows (read from
`include/boost/uuid/time_generator_v7.hpp` and `random_generator.hpp` at tag `boost-1.91.0`):

- `time_generator_v7::operator()` is `noexcept`. It reads `std::chrono::system_clock` in
  microseconds and keeps a 64-bit state of the millisecond, the microsecond within it, and a 6-bit
  counter. The millisecond fills `unix_ts_ms`; the microsecond and the counter fill `rand_a` and the
  top of `rand_b` (RFC 9562 Section 6.2, method 3 with a counter). When the clock has advanced, the
  counter resets; when the same microsecond repeats, the counter increments. Boost documents
  monotonicity for up to 64 values per microsecond. Beyond that, the 1.91.0 source carries the
  increment into the microsecond and then the millisecond field, so the embedded timestamp runs
  ahead of the clock; when the state's millisecond is ahead of the wall clock, the next call takes
  the `time_ms < (oldst >> 16)` branch and resets the state to the wall clock, which can produce a
  value smaller than the previous one without any clock rollback. Within one instance, values
  therefore strictly increase only while the clock does not go backwards and no more than 64 values
  are generated in one microsecond. When the millisecond goes backwards, the generator uses the new,
  earlier timestamp and is not monotonic across that step. The remaining 56 random bits (bytes 9 to
  15; byte 8 holds the variant and the counter) come from a ChaCha20 stream seeded from the
  operating system's entropy provider.
- `random_generator` is the same ChaCha20 stream seeded from the operating system's entropy
  provider; it produces UUIDv4 values and its call operator does not fail.
- The only operation that can fail is seeding the entropy provider, which happens when a generator
  is constructed.

The owner decided on 2026-10-25 under Issue #102:

- the production source and clock live in a dedicated adapter target, not in the composition root
  and not in `sitometron_core`; and
- the UUID generators are Boost.UUID's, used as they are; Sitometron does not assemble UUIDs itself;
  and
- after the first review (2026-10-26), the source keeps the ADR-0005 no-repeat rule literally: it
  remembers the values it issued and generates again on a repeat, instead of amending ADR-0005 with
  a probabilistic bound; and
- after the second review (2026-10-27), the remembered set has a configurable capacity with the
  default of Section 2, and a full set is reported as `IdentitySourceExhausted`, instead of an
  unbounded set or a change to the core's admission order.

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
  instance, successive values strictly increase while the system clock does not go backwards and
  no more than 64 values are generated in one microsecond; above that rate the carry-and-reset edge
  of the Context can yield a smaller value. No Sitometron component relies on this ordering; it is a
  property, not a contract that other code may depend on, and uniqueness comes from Section 2.
- Clock moving backwards: the generator takes the new, earlier timestamp. The source does not detect
  or correct this. A value generated after the step can sort before a value generated before it.
  This is acceptable because nothing orders by identifier text, and uniqueness does not depend on
  monotonicity (Section 2).
- `IdentitySourceExhausted`: returned only by the no-repeat rule of Section 2: when the generator
  repeats an issued value on all eight attempts, which means the generator is broken, or when the
  remembered set is full. The generator cannot fail per call. The source constructs its generators
  when the composition starts; if seeding the entropy provider throws, startup fails with the Issue
  #105 startup-refusal status and no listener opens.

### 2. Uniqueness across restarts

New identities rely on the millisecond timestamp and the random bits alone. The source does not read
the replayed state and does not refuse a value that an earlier run issued. Reasons:

- ADR-0005 makes the source a passive capability that the writer calls; a source that consulted
  resident snapshots would invert that direction and couple it to the Journal.
- A collision needs the same millisecond, the same sub-millisecond and counter fields, and the same
  56 random bits as a replayed Job, with probability far below any operational concern.
- The core already refuses the collision: `job_created` for an existing resident is rejected as
  `job_already_exists`, the adapter maps that to `500` `service`/`internal` (Issue #106), and no
  Journal record is written. The next create generates a new value.

Within one process, ADR-0005 requires that a source instance never issues the same value twice. The
generators alone do not guarantee this: the UUIDv7 generator strictly increases only while the
system clock does not go backwards, and the UUIDv4 and `StableId` values are random. The source
therefore keeps the rule itself:

- It remembers every value it has issued, of all three kinds, in one set.
- When a generator returns a value already in the set, the source discards it and generates again.
  A generation makes at most eight attempts, the first included; if all eight return issued values,
  the source returns `IdentitySourceExhausted`, because such a generator is broken.
- The set has a capacity, configured at composition with the default 1,048,576 identities (16 MiB of
  values; about 50 MiB with container overhead). When the set is full, the source returns
  `IdentitySourceExhausted` without generating. The core turns either exhaustion into its
  fail-closed latch (ADR-0003), readiness becomes false, and the daemon is restarted.

The capacity bounds memory because the core generates the three identities of a creation before it
decides admission, so a refused creation also adds three values, and `max_jobs` bounds residents,
not refused requests. The default is 1,024 times the largest `max_jobs` (1,024), so a daemon
reaches it only when a trusted loopback client keeps creating against a full daemon for a very long
time. The set is kept for the whole lifetime of the source instance: retiring a Job (H7, Issue #103)
does not remove its identities from the set, because a removed value could be reissued by the same
instance. Values are forgotten only when the instance ends, that is, at restart. The
capacity is not a startup option of Issue #105; the implementation Issue decides whether it is a
constructor argument only or also a command-line option.

### 3. UUIDv4 for Worker identity

The source generates the Worker identity with one `boost::uuids::random_generator` instance owned
for the process lifetime, formatted as canonical text. The value identifies a Worker and nothing
else; ADR-0004's rule stands that a UUID is never a credential, and Issue #15 keeps the Worker
bearer token a separate 256-bit secret.

### 4. `StableId` for launch operations

The launch-operation identity is the canonical text of a UUIDv7 drawn from the same
`time_generator_v7` instance as Section 1: 36 characters of lowercase hexadecimal digits and
hyphens, which satisfies the ADR-0002 grammar `^[A-Za-z0-9][A-Za-z0-9._:-]*$` with 1 to 128
characters. It needs no separate launch counter, unlike the skeleton's `launch-N`, which restarts at
1 after every restart and would collide with replayed launch operations; within the process it is
covered by the shared remembered set and retry rule of Section 2.

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
permitted as its implementation. The identity-source translation unit may not include `<random>`
or `<chrono>` and may not name `std::random_device`, `std::chrono::system_clock`, or an operating
system entropy or time call; only the clock translation unit includes `<chrono>`. The target adds
no manifest port. Its public headers expose only
Sitometron-owned types, and it links `Boost::uuid` privately. The source's constructor takes its
generators through two Sitometron-owned abstract interfaces, one that yields a UUIDv7 and one that
yields a UUIDv4, plus the set capacity; a factory in the same target constructs the production
source with Boost's generators behind those interfaces, so no Boost type appears in a public
header, and tests supply scripted implementations. The implementation Issue adds the
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
so that each value is canonical, no identifier is issued twice by one source instance, and no
production path reads another clock or entropy source for identity.

Its named checks, Planned until the implementation Issue activates them:

- `identity_uuidv7_canonical`: ten thousand Job and Session identities and launch identities from
  one source are canonical UUIDv7 text with the RFC 9562 version and variant bits, and all distinct;
- `identity_uuidv7_monotonic_burst`: successive Job identities from one source, generated without
  sleeping, strictly increase as 128-bit values while the system clock is not moved and the measured
  rate stays at or below 64 values per microsecond; when a burst exceeds that rate, the check
  records the rate and asserts only distinctness, which the remembered set guarantees;
- `identity_uuidv4_canonical`: ten thousand Worker identities are canonical UUIDv4 text with the
  version and variant bits, and all distinct;
- `identity_stable_id_grammar`: every launch-operation identity matches the ADR-0002 `StableId`
  grammar and length;
- `identity_never_repeats_within_instance`: with an injected generator that returns a value already
  issued, the source discards it, generates again, and returns a fresh value, for each of the three
  kinds and across kinds: a UUIDv7 issued as a Job identity and then returned by the generator for a
  launch-operation identity is discarded too, because the three kinds share one set; with a
  generator that repeats on every attempt, the source returns `IdentitySourceExhausted` on the
  eighth attempt; with a set capacity of a few values, the source returns `IdentitySourceExhausted`
  once the set is full; and in both cases the core latches its failure state;
- `identity_source_never_exhausted`: the production source, with Boost's generators, returns a
  generated value for every one of ten thousand calls and never `IdentitySourceExhausted`;
- `clock_recorded_at_rfc3339`: `recorded_at` formatted from an injected `system_clock::time_point`
  is the expected UTC RFC 3339 text with millisecond precision, including the epoch, a leap day, and
  the last millisecond of a year;
- `clock_monotonic_instant`: `MonotonicInstant` computed from injected `steady_clock` readings is
  nondecreasing, starts at zero at the origin, and uses checked arithmetic;
- `identity_dependency_allowlist` and `identity_public_api_isolation`: the `sitometron_identity`
  target includes only the headers of Section 7 and exposes only Sitometron-owned types;
- `identity_source_reads_no_other_clock_or_entropy`: the scanner rejects, in the identity-source
  translation unit, `<random>`, `<chrono>`, `std::random_device`, `std::chrono::system_clock`, and
  the operating system entropy and time calls named in the scanner, so the source draws every value
  from the injected generators and nothing else;
- `identity_seed_failure_refuses_startup`: with a factory whose generator construction throws, the
  production composition refuses startup with the Issue #105 startup-refusal status and opens no
  listener; and
- `sitometrond_composes_production_identity`: the production composition constructs the real source
  and clock, in both the HTTP-enabled and HTTP-off builds, and a create on a registered test
  Application in the contract fixture commits with a canonical UUIDv7 `job_id`.

The formatting and arithmetic checks inject their time points and run no real sleeps. The source
takes its generators as constructor arguments: production injects Boost's, and the no-repeat check
injects scripted generators, so the collision path is deterministic without seeding Boost's
generators. The checks that use Boost's generators observe them as black boxes; they do not move the
system clock, so the clock-rollback behavior of Section 1 is stated from the Boost 1.91.0 source and
not tested.

Traceability from Issue #102's required decisions:

| Required decision | Section | Checks |
|---|---|---|
| 1. UUIDv7: timestamp, randomness, same-millisecond method, rollback, exhaustion | 1 | `identity_uuidv7_canonical`, `identity_uuidv7_monotonic_burst`, `identity_source_never_exhausted`, `identity_never_repeats_within_instance`; rollback is stated from the Boost source and has no check |
| 2. Uniqueness across restarts and within the process | 2 | `identity_never_repeats_within_instance`; `identity_uuidv7_canonical` (distinctness); the core's `job_already_exists` refusal, tested by Issue #106 |
| 3. UUIDv4 Worker identity | 3 | `identity_uuidv4_canonical` |
| 4. `StableId` launch identity | 4 | `identity_stable_id_grammar` |
| 5. Clock | 5 | `clock_recorded_at_rfc3339`, `clock_monotonic_instant`, `identity_source_reads_no_other_clock_or_entropy` |
| 6. Threading | 6 | stated precondition; no check, because the core already serializes the calls |
| 7. Placement and dependencies | 7 | `identity_dependency_allowlist`, `identity_public_api_isolation`, `identity_source_reads_no_other_clock_or_entropy`, `identity_seed_failure_refuses_startup`, `sitometrond_composes_production_identity` |
| 8. Tests | this section | all of the above |

## Consequences

- Good: the production daemon can create Jobs with real identities as soon as Phase 2 registers an
  Application, with no change to the core or its ports.
- Good: Sitometron implements no UUID, random, or clock algorithm; the generators are Boost's, as
  ADR-0004 chose, and the adapter is a thin wrapper that tests can observe.
- Good: the generator headers stay confined to one target that a scanner guards, as ADR-0004
  requires.
- Bad: the UUIDv7 values are not monotonic across a backwards step of the system clock or above
  64 values in one microsecond. Nothing depends on that order, and this ADR forbids adding such a
  dependency without changing it.
- Bad: the generator checks are property checks, not exact-value checks, because Boost's generators
  are not seeded from a test seam; only the no-repeat path is deterministic, through injected
  generators.
- Bad: the remembered set of issued identities grows for the life of the process, including three
  values per refused creation, up to its capacity; a daemon that reaches the capacity fails closed
  and must be restarted.
- Neutral: `sitometron_identity` is a third adapter target with its own allowlist and scanners.

## Options considered

- **Implement UUIDv7 in Sitometron with injected time and randomness**: rejected because ADR-0004
  rejected implementing UUID generation in Sitometron, and the only gain is exact-value tests of
  code that would then be Sitometron's to maintain.
- **Keep ADR-0005's no-repeat rule probabilistic, with a stated bound**: rejected because it
  amends an Accepted ADR for one clause, and remembering issued values keeps the rule literal at a
  small, bounded-per-identity cost.
- **Reuse the last timestamp and increment a counter after a backwards clock step (RFC 9562
  Section 6.2)**: rejected because Sitometron would then produce UUID values itself, which
  ADR-0004 rejected, and it does nothing for the random UUIDv4 and `StableId` values.
- **Remember issued identities without a capacity**: rejected because a trusted client that keeps
  creating against a full daemon would grow the set without bound.
- **Decide admission before generating identities, so refused creations consume none**: rejected
  here because it changes the core's creation order that ADR-0003 and Issue #80 fixed; it is a
  separate core change if the bound ever proves too tight.
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
- Requirements: `ID-001` (Normative; its checks and implementation are Planned until the
  implementation Issue activates them)
- Contract Registry: `Production identity source and clock`
- Related ADR: ADR-0002, ADR-0004, ADR-0005, ADR-0006, ADR-0010
- RFC 9562, Sections 5.4, 5.7, and 6.2; RFC 3339
- Boost.UUID 1.91.0, `time_generator_v7.hpp` and `random_generator.hpp`
