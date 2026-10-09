# Contract Registry

The Registry is the inventory of cross-component contracts and stable identifiers. Contract maturity
and implementation status are independent.

## 1. Rules

1. Register a Planned row before implementation begins.
2. Give each unresolved decision one design authority. A new surface that overlaps an existing row
   must reuse its authority or obtain an ADR explaining why it cannot.
3. An Accepted ADR may make a Contract Normative while implementation remains Planned.
4. A PR names every affected row and its maturity and implementation transitions, or gives N/A.
5. An unresolved specification section uses the standard Planned-not-normative banner.
6. A Gate's horizontal review checks every new or changed surface against this Registry.

Contract maturity values are `Planned`, `Normative`, `Deprecated`, and `Superseded`. Implementation
values are `Planned`, `In progress`, `Implemented`, and `Removed`.

## 2. Registered surfaces

| Contract surface | Maturity | Implementation | Normative or design authority | Owner |
|---|---|---|---|---|
| Core/adapter standard-library-only dependency boundary | Superseded | Removed | Superseded by Accepted [ADR-0004](adr/0004-allow-explicit-core-dependencies.md); legacy reject-all-third-party behavior retired by [Issue #17](https://github.com/tetsuh/sitometron/issues/17) | Phase 0A |
| HTTP adapter dependency boundary and allowlist | Planned | Planned | Proposed [ADR-0010](adr/0010-use-boost-beast-and-asio-for-the-external-rest-v1-adapter.md) under [Issue #90](https://github.com/tetsuh/sitometron/issues/90); the manifest and CMake integration is a later Issue | Phase 1 |
| Approved core dependency boundary and allowlist | Normative | Implemented | Accepted [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) under [Issue #15](https://github.com/tetsuh/sitometron/issues/15); implemented by [Issue #17](https://github.com/tetsuh/sitometron/issues/17) | Phase 0A |
| Core Job states and transitions | Normative | Implemented | Accepted [ADR-0002](adr/0002-define-core-job-reducer-contract.md) under [Issue #3](https://github.com/tetsuh/sitometron/issues/3) | Phase 0A |
| Core commands and rejection reasons | Normative | Implemented | Accepted [ADR-0002](adr/0002-define-core-job-reducer-contract.md) under [Issue #3](https://github.com/tetsuh/sitometron/issues/3) | Phase 0A |
| Single-state-writer ingress and critical reserve | Normative | Implemented | Accepted [ADR-0003](adr/0003-define-single-state-writer-ingress-contract.md) under [Issue #10](https://github.com/tetsuh/sitometron/issues/10); implemented by [Issue #12](https://github.com/tetsuh/sitometron/issues/12) | Phase 0A |
| Core lifecycle capability ports | Normative | Implemented | Accepted [ADR-0005](adr/0005-define-phase-0a-core-capability-port-contracts.md) under [Issue #26](https://github.com/tetsuh/sitometron/issues/26); implemented by [Issue #11](https://github.com/tetsuh/sitometron/issues/11) | Phase 0A |
| JobJournal envelope and event schemas | Normative | Implemented | Accepted [ADR-0002](adr/0002-define-core-job-reducer-contract.md) for the logical contract; Issue #11 implements the owned C++ envelope and logical fake; Issue #12 implements complete logical envelope construction, non-wrapping sequence allocation, and commit ordering; [Issue #51](https://github.com/tetsuh/sitometron/issues/51) and Accepted [ADR-0006](adr/0006-define-physical-jobjournal-durability-contract.md) own the Phase 0B physical encoding, production adapter, durability, replay, recovery, and pruning | Phase 0A / 0B |
| Physical JobJournal record encoding and segment layout | Normative | Implemented | Accepted [ADR-0006](adr/0006-define-physical-jobjournal-durability-contract.md) under [Issue #51](https://github.com/tetsuh/sitometron/issues/51); pre-existing path permissions and ancestor durability left to the operator by Accepted [ADR-0007](adr/0007-leave-pre-existing-journal-paths-to-the-operator.md) under [Issue #62](https://github.com/tetsuh/sitometron/issues/62); record codec implemented by [Issue #57](https://github.com/tetsuh/sitometron/issues/57), segment writer by [Issue #59](https://github.com/tetsuh/sitometron/issues/59), and startup validation of existing segments by [Issue #61](https://github.com/tetsuh/sitometron/issues/61) | Phase 0B |
| JobJournal startup validation, replay, and pruning | Normative | Implemented | Accepted [ADR-0006](adr/0006-define-physical-jobjournal-durability-contract.md) under [Issue #51](https://github.com/tetsuh/sitometron/issues/51); startup validation and replay implemented by [Issue #61](https://github.com/tetsuh/sitometron/issues/61), writer seeding by [Issue #64](https://github.com/tetsuh/sitometron/issues/64), and offline torn-tail quarantine and prefix pruning by [Issue #69](https://github.com/tetsuh/sitometron/issues/69) | Phase 0B |
| External REST v1 | Normative | Planned | Accepted [ADR-0008](adr/0008-define-the-external-rest-v1-job-surface.md) under [Issue #76](https://github.com/tetsuh/sitometron/issues/76) for the Job surface, supplemented by Accepted [ADR-0009](adr/0009-settle-the-external-rest-v1-gaps-found-by-the-prototype.md) under [Issue #86](https://github.com/tetsuh/sitometron/issues/86) (`API-007`), and described by [`schemas/openapi/rest-v1.openapi.json`](../schemas/openapi/rest-v1.openapi.json) under [Issue #88](https://github.com/tetsuh/sitometron/issues/88); Admission, roles, and Registry extensions are not part of the contract yet and stay with [Issue #35](https://github.com/tetsuh/sitometron/issues/35) | Phase 1 |
| Application Registry schema | Planned | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment of the pending Phase 1 Issue and ADR | Phase 1 |
| Worker HTTP v1 routes and JSON schemas | Planned | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment of the pending Phase 2 Issue and ADR | Phase 2 |
| ResourceProfile and ExecutionPolicy schemas | Planned | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment of the pending Phase 3 Issue and ADR | Phase 3 |
| Sitos adapter boundary | Planned | Planned | Upstream contracts remain prerequisites; [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment of the pending Phase 4 ADR | Phase 4 |
| Artifact REST and manifest schemas | Planned | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment of the pending Phase 5 Issue and ADR | Phase 5 |
