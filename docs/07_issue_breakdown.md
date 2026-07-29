# Issue breakdown and Phase gates

## 1. Phase mapping

| Phase | GitHub Milestone | Gate status |
|---|---|---|
| 0A | `v0.1 / P0A Bootstrap` | [Gate #1](https://github.com/tetsuh/sitometron/issues/1) open |
| 0B | `v0.1 / P0B Durability` | Gate Issue required before entry |
| 1 | `v0.1 / P1 REST & Admission` | Gate Issue required before entry |
| 2 | `v0.1 / P2 Local Worker` | Gate Issue required before entry |
| 3 | `v0.1 / P3 Resources` | Gate Issue required before entry |
| 4 | `v0.1 / P4 Sitos Integration` | Gate Issue required before entry |
| 5 | `v0.1 / P5 Artifact REST` | Gate Issue required before entry |
| 6 | `v0.1 / P6 Qualification` | Gate Issue required before entry |

Each Milestone contains exactly one `[Gate]` management Issue. It records Entry gate, horizontal
design review, Exit gate, finding dispositions and owners, and evidence. The Gate Issue closes last;
then the Milestone closes.

## 2. Phase 0A

| Issue | Scope | Status |
|---|---|---|
| [#1](https://github.com/tetsuh/sitometron/issues/1) | Phase 0A Gate | Open |
| [#2](https://github.com/tetsuh/sitometron/issues/2) | Repository and dependency-free core bootstrap | Completed |
| [#3](https://github.com/tetsuh/sitometron/issues/3) | Job state, event, and reducer ADR | Open |
| [#4](https://github.com/tetsuh/sitometron/issues/4) | Repository governance alignment | Open |

Phase 0A completes only when Gate #1 links evidence for:

- green Linux and Windows CI;
- enforced standard-library-only core isolation;
- buildable minimal daemon;
- normative machine-readable Job state, event, command, and rejection contracts;
- deterministic fake clock, Journal, ApplicationRunner, and UUID generator;
- the first fake-driven Job lifecycle vertical slice;
- transition, Journal-failure, stop-race, late-event, and first-cause tests;
- executable clean-room, ADR, Contract Registry, Issue, PR, and Gate workflows;
- no production adapter dependency in core.

## 3. Later Phase ownership

> **Planned, not yet normative:** Each future Phase Gate and its design Issues own the mechanisms
> below. Implementers must not treat this outline as a finalized contract.

- Phase 0B owns the logger durability spike and production JobJournal foundation.
- Phase 1 owns external REST, Admission, and Application Registry contracts.
- Phase 2 owns Worker protocol schemas and local process containment.
- Phase 3 owns topology, ResourceProfile, scheduling, and reservation contracts.
- Phase 4 owns the installed Sitos adapter and required upstream contract gates.
- Phase 5 owns Artifact REST and terminal-manifest contracts.
- Phase 6 owns cross-platform qualification and release decisions.

Create implementation Issues only after the owning design authority registers affected contract
surfaces and names required tests.
