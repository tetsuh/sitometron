# ADR-0001: Bootstrap a standard-library-only C++20 core

## Status

Accepted — 2026-07-29

## Owners

Sitometron maintainers

## Context

Sitometron needs deterministic lifecycle, admission, and scheduling policy that can be tested
without HTTP, persistence, process, hardware, Sitos, Zenoh, Holoscan, Python, or logger frameworks.
The project must support Linux and Windows while keeping application and product-specific behavior
outside its core.

An empty repository cannot apply the normal rule that an ADR lands before its implementation
because no default branch, workflow, or ADR location exists yet. The initial repository commit is a
one-time governance bootstrap exception and includes this Accepted decision with its minimal build
skeleton. Later contract decisions follow the normal separate-ADR process.

## Decision

- Use C++20 with CMake 3.28 or later and Ninja presets.
- Build `sitometron_core` as an internal static library with no ABI-stability promise in v0.1.
- Limit `sitometron_core` to the C++ standard library.
- Put HTTP, Journal backend, process containment, topology, Sitos, and logging in adapter targets.
- Compose adapters only in `sitometrond`.
- Enable exceptions and RTTI.
- Reject in-source builds.
- Compile project code with warnings as errors.
- Use fake-driven deterministic unit tests without real sleeps.
- Use vcpkg manifest mode for later C++ dependencies, with no default dependency in the bootstrap.

## Consequences

Core ports must use Sitometron-owned domain types rather than adapter-library types. Introducing a
new standard header in core requires updating the reviewed dependency-isolation allowlist. Adding a
third-party dependency to core requires a superseding ADR.

The bootstrap does not complete Phase 0A. Machine-readable state/event contracts, reducer behavior,
fake Journal/clock/runner, and the first Job lifecycle vertical slice remain gated work.
