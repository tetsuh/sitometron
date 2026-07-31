# ADR-0004: Allow explicit dependencies in the C++20 core

## Status

Proposed — 2026-07-31

## Context

ADR-0001 bootstrapped `sitometron_core` as a C++20 standard-library-only internal static library.
That boundary kept HTTP, persistence, process control, topology, Sitos, logging, Python, and other
framework behavior out of domain policy while the repository and core contracts were being created.
It also required a superseding ADR before any third-party dependency could enter the core.

Accepted ADR-0002 now requires the core Job reducer implementation to validate strict JSON-derived
DTOs, canonical UUID versions and variants, and SHA-256 over the exact original UTF-8 JSON bytes.
Keeping ADR-0001 unchanged would require Sitometron to implement and maintain JSON parsing, UUID
parsing, and SHA-256. Those implementations would add correctness and review risk without creating
product-specific value.

The pinned vcpkg builtin baseline
`40f3c709db80acf154ac4b17a1f83c564ebd022e` provides suitable cross-platform components:

- `nlohmann-json` 3.12.0#2;
- `boost-uuid` 1.91.0; and
- `boost-hash2` 1.91.0.

Issue #15 records the owner-selected libraries and the requirement that dependency-owned types must
not become Sitometron domain contracts. This ADR changes only the dependency boundary. It must
preserve the remaining bootstrap decisions and must not change ADR-0002 lifecycle semantics.

## Decision

If accepted, this ADR supersedes ADR-0001 as a whole. It replaces only the standard-library-only
restriction and restates every unaffected bootstrap decision below so that their authority remains
traceable.

### Preserved bootstrap decisions

- Use C++20 with CMake 3.28 or later and Ninja presets.
- Build `sitometron_core` as an internal static library with no ABI-stability promise in v0.1.
- Keep domain ports and public APIs in Sitometron-owned types.
- Keep HTTP, Journal backends, process containment, topology, Sitos, logging, other I/O, and
  framework behavior in adapter targets.
- Compose concrete adapters only in `sitometrond`.
- Enable exceptions and RTTI.
- Reject in-source builds.
- Compile project code with warnings as errors without imposing project warnings on dependencies.
- Use fake-driven deterministic unit tests without real sleeps when explicit inputs can express the
  behavior.
- Use manifest-mode vcpkg with pinned acquisition and no package-manager or network invocation from
  project CMake or runtime code.

### Closed direct dependency allowlist

Only these manifest ports, direct CMake targets, and source-level facilities are authorized:

| Manifest port | Pinned-baseline version | Direct CMake target | Authorized source-level use |
|---|---:|---|---|
| `nlohmann-json` | 3.12.0#2 | `nlohmann_json::nlohmann_json` | `<nlohmann/json.hpp>` for JSON syntax parsing and value access |
| `boost-uuid` | 1.91.0 | `Boost::uuid` | `<boost/uuid/uuid.hpp>`, `<boost/uuid/string_generator.hpp>`, and `<boost/uuid/uuid_io.hpp>` for values, parsing, inspection, and canonical formatting; `<boost/uuid/time_generator_v7.hpp>` and `<boost/uuid/random_generator.hpp>` only behind the later owning generator adapter |
| `boost-hash2` | 1.91.0 | `Boost::hash2` | `<boost/hash2/sha2.hpp>` for SHA-256 over exact bytes |

The intended direct CMake integration is:

```cmake
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS uuid hash2)

target_link_libraries(
  sitometron_core
  PRIVATE
    nlohmann_json::nlohmann_json
    Boost::uuid
    Boost::hash2)
```

The integration Issue may refine placement into private implementation targets if that preserves the
same direct allowlist and public boundary. Because `sitometron_core` is static, CMake may retain
private dependencies as `$<LINK_ONLY:...>` metadata for final linking. That metadata does not
permit dependency-owned includes or types in public core headers.

The baseline-resolved transitive Boost ports and any system link requirements are permitted only as
opaque upstream implementation prerequisites. Sitometron core source must not directly include,
link, or use a transitive package unless a later allowlist and ADR update explicitly authorize it.
Dependency and license reporting still covers the complete resolved closure. This ADR does not
approve arbitrary Boost components or arbitrary third-party dependencies.

### Public type and validation boundary

Public headers under `include/sitometron/core` expose Sitometron-owned UUID, digest, DTO, error, and
result types. Dependency-owned types remain private implementation details.

nlohmann/json provides JSON syntax parsing and value access only. Sitometron-owned validation remains
responsible for unknown fields, closed enums, ranges, identity rules, cross-field invariants, and
mapping dependency exceptions into Sitometron-owned results. A JSON Schema runtime engine is not
introduced.

For ADR-0002 resolved allocations, the implementation retains the original UTF-8 JSON byte
sequence, validates its syntax, and applies SHA-256 to those exact bytes. It does not parse and
reserialize before hashing. A raw NUL byte makes the JSON text invalid and is rejected as a syntax
error; a JSON string represents U+0000 through its original `\\u0000` escape bytes. Independently,
the SHA-256 primitive is length-safe for arbitrary byte sequences, including embedded NUL bytes.
The result is a Sitometron-owned lowercase 64-hex digest; Boost digest types are not stored in
snapshots, DTOs, or public APIs.

Boost.UUID parsing does not define Sitometron's accepted text grammar. Sitometron enforces lowercase
canonical `8-4-4-4-12` text and rejects braces, uppercase, compact forms, wrong versions, and wrong
RFC variants even if an upstream parser accepts them. Job and Session IDs are UUIDv7, Worker IDs are
UUIDv4, and launch-operation IDs remain ADR-0002 `stableId` values unless a later owning ADR changes
that contract.

This ADR selects Boost.UUID but does not define production generation semantics. UUIDv7 clock,
random-source, rollback, monotonicity, and same-millisecond rules belong to the later ID-generator
Issue. Deterministic fake generation does not call Boost generators. UUIDs are identifiers and are
never used as the 256-bit Worker bearer credential, whose CSPRNG mechanism requires separate review.

### Requirements and checks

ADR-0004 replaces `NFR-002` with this requirement when accepted:

- `NFR-005` (MUST): Keep `sitometron_core` dependency-minimal. Permit only dependencies explicitly
  named by the accepted core allowlist and pinned manifest; expose Sitometron-owned public types;
  treat transitive packages as opaque prerequisites; and mechanically reject unapproved direct
  targets, includes, and public dependency leakage.

The following `NFR-005` checks are stable but remain Planned until the separate integration Issue
implements and activates them:

- `core_dependency_allowlist`;
- `core_dependency_rejects_unapproved_target`;
- `core_dependency_rejects_unapproved_private_include`;
- `core_public_api_dependency_isolation`; and
- `core_dependency_api_smoke`.

`core_dependency_api_smoke` covers JSON, UUID, and SHA-256 APIs, official SHA-256 known-answer
vectors, arbitrary-byte embedded-NUL and explicit-length hashing, rejection of raw-NUL JSON text,
and lowercase 64-hex over the original valid JSON bytes. Linux and Windows CI must configure,
compile, link, and run the applicable checks without hidden package acquisition.

While this ADR is Proposed, ADR-0001 and `NFR-002` remain Normative and their existing checks and
the standard-library-only rule in `AGENTS.md` remain Active. If ADR-0004 is accepted, ADR-0001 and
`NFR-002` become Superseded, `NFR-005` becomes Normative with Planned implementation, and unchanged
`NFR-001`, `NFR-003`, and `NFR-004` move to ADR-0004 authority. The acceptance commit also updates
`AGENTS.md` to the closed allowlist and synchronizes Gate #1, Issue #9, and the Phase 0A label
description without implying implementation. Only the later integration Issue activates `NFR-005`
checks, retires the legacy `NFR-002` checks, updates the manifest/CMake/CI, and promotes the new
Registry row to Implemented.

### Acquisition and attribution

The builtin baseline and a separate vcpkg tool checkout commit are pinned independently. Developers
and CI pre-provision `VCPKG_ROOT`; project CMake does not clone, bootstrap, update, or invoke vcpkg.
No `FetchContent`, vendored dependency source, Conan, hidden network access, or runtime acquisition is
introduced.

nlohmann/json uses the MIT License. Boost.UUID, Boost.Hash2, and their Boost dependency closure use
the Boost Software License 1.0. Packaging and source/generated distributions retain required MIT
notices and applicable Boost terms for the complete resolved closure. This decision does not create
an Apache-style project `NOTICE` file unless a later packaging requirement makes one necessary.

## Consequences

- Good: Sitometron does not implement its own JSON parser, UUID parser, or SHA-256 algorithm.
- Good: The selected components are available at one pinned cross-platform vcpkg baseline.
- Good: Runtime behavior remains deterministic because parsing and hashing consume explicit inputs;
  production UUID generation remains outside the pure reducer.
- Good: Public domain contracts remain owned by Sitometron rather than dependency APIs.
- Bad: `sitometron_core` is no longer standard-library-only and builds require a pre-provisioned
  vcpkg dependency installation.
- Bad: Boost component ports resolve additional transitive packages that must be reported and kept
  opaque to core source.
- Bad: Header-only dependencies can increase compile time and require explicit public-header
  leakage checks.
- Neutral: ADR acceptance alone does not integrate dependencies or authorize Issue #9. A separate
  implementation Issue must merge Linux/Windows integration evidence first.
- Neutral: Future direct core dependencies still require an allowlist and ADR update.

## Options considered

- **Retain the standard-library-only core**: rejected because handwritten JSON, UUID, and SHA-256
  implementations add correctness and convergence risk without product value.
- **Use POCO Foundation for UUIDv7 and SHA-256**: rejected because a compiled, broad Foundation
  dependency and its base closure are disproportionate when Sitometron does not use POCO for HTTP.
- **Use Boost.UUID with PicoSHA2**: rejected because Boost.Hash2 provides the required digest under
  the same Boost release and license family without adding another implementation ecosystem.
- **Use OpenSSL for SHA-256**: rejected because the crypto/runtime dependency is unnecessarily broad
  for non-secret exact-byte integrity hashing in Phase 0A.
- **Use `stduuid`**: rejected because the evaluated version does not provide the required UUIDv7
  support.
- **Implement JSON, UUID, or SHA-256 in Sitometron**: rejected because these standardized primitives
  require avoidable implementation and review effort.
- **Allow all Boost components or arbitrary third-party dependencies**: rejected because it would
  erase the mechanically enforced domain boundary.
- **Expose vendor types in public core APIs**: rejected because dependency APIs would become stable
  Sitometron contracts and increase coupling.

## References

- Issue #15
- PR #16
- Issue #9
- Gate #1
- Requirements: `NFR-001`, `NFR-002`, `NFR-003`, `NFR-004`, proposed `NFR-005`
- Contract Registry: `Core/adapter standard-library-only dependency boundary`, proposed `Approved
  core dependency boundary and allowlist`
- Related ADRs: [ADR-0001](0001-bootstrap-a-stdlib-only-cpp20-core.md),
  [ADR-0002](0002-define-core-job-reducer-contract.md)
- RFC 9562: UUIDs
- Pinned vcpkg builtin baseline: `40f3c709db80acf154ac4b17a1f83c564ebd022e`
