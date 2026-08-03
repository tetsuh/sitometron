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
6. A Milestone horizontal review checks every new or changed surface against this Registry.

Contract maturity values are `Planned`, `Normative`, `Deprecated`, and `Superseded`. Implementation
values are `Planned`, `In progress`, `Implemented`, and `Removed`.

## 2. Registered surfaces

| Contract surface | Maturity | Implementation | Normative or design authority | Owner |
|---|---|---|---|---|
| Core/adapter standard-library-only dependency boundary | Superseded | Removed | Superseded by Accepted [ADR-0004](adr/0004-allow-explicit-core-dependencies.md); legacy reject-all-third-party behavior retired by [Issue #17](https://github.com/tetsuh/sitometron/issues/17) | Phase 0A |
| Approved core dependency boundary and allowlist | Normative | Implemented | Accepted [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) under [Issue #15](https://github.com/tetsuh/sitometron/issues/15); implemented by [Issue #17](https://github.com/tetsuh/sitometron/issues/17) | Phase 0A |
| Core Job states and transitions | Normative | Implemented | Accepted [ADR-0002](adr/0002-define-core-job-reducer-contract.md) under [Issue #3](https://github.com/tetsuh/sitometron/issues/3) | Phase 0A |
| Core commands and rejection reasons | Normative | Implemented | Accepted [ADR-0002](adr/0002-define-core-job-reducer-contract.md) under [Issue #3](https://github.com/tetsuh/sitometron/issues/3) | Phase 0A |
| Single-state-writer ingress and critical reserve | Normative | Planned | Accepted [ADR-0003](adr/0003-define-single-state-writer-ingress-contract.md) under [Issue #10](https://github.com/tetsuh/sitometron/issues/10) | Phase 0A |
| Core lifecycle capability ports | Planned | Planned | Proposed [ADR-0005](adr/0005-define-phase-0a-core-capability-port-contracts.md) under [Issue #26](https://github.com/tetsuh/sitometron/issues/26); Issue #11 implementation pending | Phase 0A |
| JobJournal envelope and event schemas | Normative | Planned | Accepted [ADR-0002](adr/0002-define-core-job-reducer-contract.md) for the logical contract; Phase 0B ADR pending for physical durability | Phase 0A / 0B |
| External REST v1 | Planned | Planned | Pending Phase 1 Issue and ADR | Phase 1 |
| Application Registry schema | Planned | Planned | Pending Phase 1 Issue and ADR | Phase 1 |
| Worker HTTP v1 routes and JSON schemas | Planned | Planned | Pending Phase 2 Issue and ADR | Phase 2 |
| ResourceProfile and ExecutionPolicy schemas | Planned | Planned | Pending Phase 3 Issue and ADR | Phase 3 |
| Sitos adapter boundary | Planned | Planned | Upstream contracts plus pending Phase 4 ADR | Phase 4 |
| Artifact REST and manifest schemas | Planned | Planned | Pending Phase 5 Issue and ADR | Phase 5 |
