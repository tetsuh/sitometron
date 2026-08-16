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
| [#1](https://github.com/tetsuh/sitometron/issues/1) | Phase 0A Gate | Open; closes last |
| [#2](https://github.com/tetsuh/sitometron/issues/2) | Repository and dependency-free bootstrap baseline | Completed |
| [#3](https://github.com/tetsuh/sitometron/issues/3) | Job state, event, and reducer ADR | Completed |
| [#4](https://github.com/tetsuh/sitometron/issues/4) | Repository governance alignment | Completed |
| [#7](https://github.com/tetsuh/sitometron/issues/7) | Pinned Draft 2020-12 schema tooling | Completed |
| [#9](https://github.com/tetsuh/sitometron/issues/9) | Dependency-minimal pure Job reducer | Completed |
| [#10](https://github.com/tetsuh/sitometron/issues/10) | Single-state-writer ingress ADR | Completed |
| [#11](https://github.com/tetsuh/sitometron/issues/11) | Lifecycle capability ports and deterministic fakes | Completed |
| [#12](https://github.com/tetsuh/sitometron/issues/12) | Fake-driven Job lifecycle and private single writer | Completed |
| [#13](https://github.com/tetsuh/sitometron/issues/13) | Deterministic adverse/race qualification | Completed |
| [#14](https://github.com/tetsuh/sitometron/issues/14) | Final Phase 0A CI and Gate evidence | Open |
| [#15](https://github.com/tetsuh/sitometron/issues/15) | Explicit core dependency allowlist ADR | Completed |
| [#17](https://github.com/tetsuh/sitometron/issues/17) | Approved dependency-boundary integration | Completed |
| [#20](https://github.com/tetsuh/sitometron/issues/20) | SonarQube Cloud automatic analysis | Completed |
| [#22](https://github.com/tetsuh/sitometron/issues/22) | Behavior-preserving reducer complexity refactor | Completed |
| [#23](https://github.com/tetsuh/sitometron/issues/23) | Post-implementation documentation alignment | Completed |
| [#25](https://github.com/tetsuh/sitometron/issues/25) | Canonical developer bootstrap | Completed |
| [#26](https://github.com/tetsuh/sitometron/issues/26) | Lifecycle capability-port ADR | Completed |
| [#36](https://github.com/tetsuh/sitometron/issues/36) | Deterministic Phase 0A policy CTests | Open |
| [#37](https://github.com/tetsuh/sitometron/issues/37) | clang-tidy and sanitizer CI | Open |
| [#38](https://github.com/tetsuh/sitometron/issues/38) | Pinned Phase 0A secret scanning | Open |
| [#39](https://github.com/tetsuh/sitometron/issues/39) | Phase 0A documentation and governance validation | Open |
| [#40](https://github.com/tetsuh/sitometron/issues/40) | Post-Phase-0A Planned-authority assignment | Open; first remaining child |
| [#41](https://github.com/tetsuh/sitometron/issues/41) | Phase 0A clang-tidy baseline cleanup | Open |

The current remaining order is:

```text
#40 Planned-authority links -> Gate #1 horizontal-review acceptance
  -> #36 policy CTests -> #41 clang-tidy baseline cleanup -> #37 clang-tidy/sanitizer CI
  -> #38 secret scanning -> #39 documentation/governance validation
  -> #14 final CI/Gate evidence -> #1 closes
```

Issue #35 is the milestone-external assignment/deferral tracker for post-Phase-0A Planned sections;
it is not a Phase 0A implementation item.

The completed shared-mechanism order is:

```text
#3 / ADR-0002 -------------------------------> #9 reducer -> #22 reducer refactor
#15 / ADR-0004 -> #17 dependency integration -+
#10 / ADR-0003 -------------------------------+-> #12 lifecycle/single writer -> #13 qualification
#26 / ADR-0005 -> #11 ports and fakes --------+
```

Issue #31 is a separate private-orchestrator maintainability follow-up. It is not in this Milestone or
Gate dependency graph unless a later owner decision promotes it.

Phase 0A completes only when Gate #1 links evidence for:

- green Linux and Windows CI and the remaining Phase 0A analysis/policy lanes;
- the implemented ADR-0004/`NFR-005` dependency-minimal core boundary;
- a buildable minimal daemon with no production adapter enabled;
- Normative and Implemented machine-readable Job contracts, lifecycle ports, bounded ingress, and
  complete logical JobJournal envelope/order;
- the fake-driven Job lifecycle plus deterministic failure, race, callback, shutdown, late-event,
  first-cause, and capacity qualification;
- current descriptive documentation and a canonical supported developer bootstrap; and
- executable clean-room, architecture, dependency, ADR, Contract Registry, Issue, PR, and Gate
  workflows.

Physical JobJournal durability and production adapters are not Phase 0A exit criteria.

## 3. Later Phase ownership

> **Planned, not yet normative:** [Issue #35](https://github.com/tetsuh/sitometron/issues/35)
> tracks assignment of each future Phase Gate and its design authorities for the mechanisms below.
> Implementers must not treat this outline as a finalized contract.

- Phase 0B owns the logger durability spike and production JobJournal foundation.
- Phase 1 owns external REST, Admission, and Application Registry contracts.
- Phase 2 owns Worker protocol schemas and local process containment.
- Phase 3 owns topology, ResourceProfile, scheduling, and reservation contracts.
- Phase 4 owns the installed Sitos adapter and required upstream contract gates.
- Phase 5 owns Artifact REST and terminal-manifest contracts.
- Phase 6 owns cross-platform qualification and release decisions.

Create implementation Issues only after the owning design authority registers affected contract
surfaces and names required tests.
