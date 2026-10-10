# Issue breakdown and Phase gates

## 1. Phase mapping

| Phase | GitHub Milestone | Gate status |
|---|---|---|
| 0A | `v0.1 / P0A Bootstrap` | [Gate #1](https://github.com/tetsuh/sitometron/issues/1) closed on 2026-09-24 |
| 0B | `v0.1 / P0B Durability` | [Gate #50](https://github.com/tetsuh/sitometron/issues/50) closed on 2026-10-04 |

Later work is planned one working increment at a time and has no Gate prepared in advance. A
`[Gate]` Issue is created only when a contract must be frozen before implementation, as
[the development workflow](development_workflow.md#9-phase-and-milestone-gates) states. A Gate
records Entry gate, horizontal design review, Exit gate, finding dispositions and owners, and
evidence; it closes last, then its Milestone closes.

## 2. Phase 0A

| Issue | Scope | Status |
|---|---|---|
| [#1](https://github.com/tetsuh/sitometron/issues/1) | Phase 0A Gate | Closed on 2026-09-24 |
| [#2](https://github.com/tetsuh/sitometron/issues/2) | Repository and dependency-free bootstrap baseline | Completed |
| [#3](https://github.com/tetsuh/sitometron/issues/3) | Job state, event, and reducer ADR | Completed |
| [#4](https://github.com/tetsuh/sitometron/issues/4) | Repository governance alignment | Completed |
| [#7](https://github.com/tetsuh/sitometron/issues/7) | Pinned Draft 2020-12 schema tooling | Completed |
| [#9](https://github.com/tetsuh/sitometron/issues/9) | Dependency-minimal pure Job reducer | Completed |
| [#10](https://github.com/tetsuh/sitometron/issues/10) | Single-state-writer ingress ADR | Completed |
| [#11](https://github.com/tetsuh/sitometron/issues/11) | Lifecycle capability ports and deterministic fakes | Completed |
| [#12](https://github.com/tetsuh/sitometron/issues/12) | Fake-driven Job lifecycle and private single writer | Completed |
| [#13](https://github.com/tetsuh/sitometron/issues/13) | Deterministic adverse/race qualification | Completed |
| [#14](https://github.com/tetsuh/sitometron/issues/14) | Final Phase 0A CI and Gate evidence | Completed |
| [#15](https://github.com/tetsuh/sitometron/issues/15) | Explicit core dependency allowlist ADR | Completed |
| [#17](https://github.com/tetsuh/sitometron/issues/17) | Approved dependency-boundary integration | Completed |
| [#20](https://github.com/tetsuh/sitometron/issues/20) | SonarQube Cloud automatic analysis | Completed |
| [#22](https://github.com/tetsuh/sitometron/issues/22) | Behavior-preserving reducer complexity refactor | Completed |
| [#23](https://github.com/tetsuh/sitometron/issues/23) | Post-implementation documentation alignment | Completed |
| [#25](https://github.com/tetsuh/sitometron/issues/25) | Canonical developer bootstrap | Completed |
| [#26](https://github.com/tetsuh/sitometron/issues/26) | Lifecycle capability-port ADR | Completed |
| [#36](https://github.com/tetsuh/sitometron/issues/36) | Deterministic Phase 0A policy CTests | Completed |
| [#37](https://github.com/tetsuh/sitometron/issues/37) | clang-tidy and sanitizer CI | Completed |
| [#38](https://github.com/tetsuh/sitometron/issues/38) | Pinned Phase 0A secret scanning | Completed |
| [#39](https://github.com/tetsuh/sitometron/issues/39) | Phase 0A documentation and governance validation | Completed |
| [#40](https://github.com/tetsuh/sitometron/issues/40) | Post-Phase-0A Planned-authority assignment | Completed |
| [#41](https://github.com/tetsuh/sitometron/issues/41) | Phase 0A clang-tidy baseline cleanup | Completed |

All Phase 0A Issues are complete, and Gate #1 closed on 2026-09-24.

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

- Phase 0B owns the production JobJournal foundation. Gate #50 tracks it, and Issue #51 owns the
  physical JobJournal design through Accepted ADR-0006. The logger is deferred by owner decision
  (2026-10-04, Gate #50): the durability spike and the ADR-0004 dependency decision that ADR-0006
  Section 9 requires are filed when a logger is needed. Gate #50 closed on 2026-10-04; the design
  inputs it handed over to later Phases are tracked in
  [Issue #73](https://github.com/tetsuh/sitometron/issues/73).
- Phase 1 owns external REST, Admission, and Application Registry contracts.
  [Issue #76](https://github.com/tetsuh/sitometron/issues/76) owns the External REST v1 Job surface
  through Accepted ADR-0008, and [Issue #86](https://github.com/tetsuh/sitometron/issues/86)
  settles the gaps the prototype found through Accepted ADR-0009.
  [Issue #90](https://github.com/tetsuh/sitometron/issues/90) chooses the HTTP library of the
  adapter through Accepted ADR-0010, and
  [Issue #94](https://github.com/tetsuh/sitometron/issues/94) its JSON library through Accepted
  ADR-0011. No Gate exists until the first production implementation Issue of Phase 1 is about to
  start.
- Phase 2 owns Worker protocol schemas and local process containment.
- Phase 3 owns topology, ResourceProfile, scheduling, and reservation contracts.
- Phase 4 owns the installed Sitos adapter and required upstream contract gates.
- Phase 5 owns Artifact REST and terminal-manifest contracts.
- Phase 6 owns cross-platform qualification and release decisions.

Create implementation Issues only after the owning design authority registers affected contract
surfaces and names required tests.
