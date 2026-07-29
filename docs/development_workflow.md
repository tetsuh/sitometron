# Development workflow

This document defines Sitometron's branching, ticket-driven development, test-driven development,
pull-request, and merge rules. Explicit rules take precedence over implicit convention.

## 1. Trunk-based branching

`main` must remain green and releasable. Intentional RED commits may exist only inside a feature
branch's development history. Direct pushes to `main` are prohibited.

Every change uses one short-lived branch:

```text
feat/<issue-number>-<short-kebab-description>
```

The prefix is always `feat/`; the Issue label and commit type describe the kind of change. Examples:

```text
feat/3-job-state-event-contract
feat/4-align-development-governance
```

Do not create long-lived development branches. Releases are tags on `main` unless a later ADR adopts
a release-branch policy.

## 2. Ticket-driven development

**No Ticket, No Commit.** Every change starts from a GitHub Issue.

The normal unit is:

```text
1 Issue = 1 feat branch = 1 pull request
```

Before work begins, the Issue must provide:

- applicable reference documents;
- applicable Requirement IDs from [`01_requirements.md`](01_requirements.md), or an N/A reason;
- target files or components;
- verifiable acceptance criteria;
- dependencies and blockers;
- affected Contract Registry rows with maturity and implementation transitions, or an N/A reason;
- an ADR decision or an explicit ADR-not-required reason;
- clean-room provenance confirmation.

Do not include unrelated fixes in the same PR. File a separate Issue for a newly discovered problem.
A reviewer may permit a trivial correction that cannot reasonably be separated.

A design, Proposal, or ADR Issue may evolve until the owner declares its scope ready for
implementation. An implementation Issue's checklist freezes when implementation begins. After that
point, clarification may improve wording without changing meaning; a material scope change requires
an owner decision, renewed review, and an Issue split when the original unit is no longer cohesive.
Copy the frozen checklist into the PR and track implementation progress there. The Issue body remains
the scope authority rather than a progress display.

The PR body uses `Closes #NN`. Merge closes the Issue only after the owner's merge decision.

## 3. Commit messages

Use Conventional Commits with an Issue reference:

```text
<type>(<scope>): <imperative summary> (#<issue>)

- <what changed and why>
- <verification or constraint>
```

Allowed initial types are `feat`, `fix`, `docs`, `test`, `refactor`, `build`, `ci`, and `chore`.
The English header starts with a lowercase imperative summary, has no trailing period, and should not
exceed 72 characters. The optional body contains bullet-list items. Use a
`BREAKING CHANGE: <description>` footer when applicable.

Examples:

```text
test(core): add rejected-transition vectors (#3)

docs(workflow): align governance with Sitos (#4)
```

## 4. Test-driven development

Production behavior follows RED, GREEN, REFACTOR:

1. **RED**: add an acceptance or reproduction test and confirm the expected failure.
2. **GREEN**: add the minimum implementation that makes the test pass.
3. **REFACTOR**: improve structure without weakening assertions.

The PR records concise structured RED evidence:

- the command that was run;
- the failing test name;
- the expected reason for failure;
- one representative failure-message line;
- a complete CI-log link only when the excerpt is insufficient.

It also records the final passing command. A test that unexpectedly passes during RED must be
reviewed before implementation continues. Production code without tests is prohibited.
Documentation, build metadata, and non-executable examples may use applicable validation instead of
a RED test.

An intentional RED commit is permitted in feature-branch history when it contains only the expected
acceptance or reproduction-test failure. It must not disguise unrelated compile errors or incomplete
changes, and a corresponding GREEN commit must follow before merge. The PR head and the resulting `main` integration commit must pass all known required checks.

Each Requirement ID maps to required test names in
[`06_build_test_packaging.md`](06_build_test_packaging.md). Tests may iterate machine-readable
vectors internally without creating another public fixture-ID mapping. A required test-name change
updates that table and is reviewed against the owning Requirement.

Tests must avoid real sleeps when fake clocks or deterministic event injection can express the
behavior. Race tests define the accepted ordering rather than depending on scheduler timing.

## 5. Architecture decisions

An ADR is required before changing:

- lifecycle, reducer, or terminal-outcome semantics;
- wire formats, routes, public APIs, or stable identifiers;
- threading, ordering, retry, timeout, or queue guarantees;
- durability, recovery, or persistence authority;
- authentication, authorization, or process-containment boundaries;
- supported platforms, build foundations, or major dependencies;
- a contract surface that overlaps an existing Contract Registry row.

ADRs normally use a separate PR from production implementation. A small decision may be bundled
with its implementation PR when the decision and implementation have one Issue scope and remain
independently reviewable; include `[ADR]` in that PR title. See
[the ADR process](10_adr_process.md).

## 6. Contract registration

Register a Planned Contract Registry row before implementation. Contract maturity and
implementation status are independent. Every Issue and PR identifies affected rows or gives an N/A
reason.

A new or materially changed mechanism shared by two or more Issues needs one design authority. Reuse
an existing owner or Accepted ADR when one exists; otherwise create a unifying design Issue before
the affected implementations proceed.

See [the Contract Registry](08_contract_registry.md).

## 7. Pull requests

One PR implements one Issue. Keep the diff reviewable; 500 changed implementation and test lines is
a guideline for considering a split, not an automatic rejection threshold.

A PR records:

- Phase and Gate Issue;
- related Issue and ADR;
- `Closes #NN`;
- applicable Requirement IDs or N/A;
- affected Contract Registry rows or N/A;
- acceptance-criteria checklist;
- RED evidence when required;
- build, test, formatting, static-analysis, and platform evidence;
- clean-room provenance confirmation;
- unresolved risks and follow-up Issues.

CI and required review conversations must be complete before the PR is presented as merge-ready.
Passing CI or an automated review never authorizes merge. Independent secondary review is optional,
not a merge gate; the owner requests it when its expected risk reduction justifies human time or AI
subscription/token use. Findings from any requested review still require an explicit disposition.

## 8. Owner-only merge authority

The repository owner makes every merge decision and normally performs the merge. The owner may
delegate one merge action under the conditions below.

Coding agents, automation, and other contributors must not:

- merge a PR without an explicit owner instruction for that PR;
- enable or schedule auto-merge;
- treat passing checks or approvals as implicit merge authorization;
- choose the final merge method on the owner's behalf.

They normally stop after preparing a merge-ready PR, responding to review, and reporting evidence
and residual risks. A coding agent may execute the merge when the owner explicitly instructs it to
merge that PR at its current head. The instruction is one-time and expires if a new commit changes
the head or a new blocking finding appears. The owner must authorize the new state before an agent
merges it.

The default method is a normal merge commit. The owner may choose squash merge when retaining the
branch's individual commits would not improve `main` history. An unqualified owner instruction to
merge uses the normal merge method; squash requires the owner to select it. Normal merge uses
GitHub's generated default merge commit message, which the owner does not need to edit. Rebase merge
is not part of the current workflow.

Normal merge preserves RED, GREEN, and REFACTOR commits as non-first-parent development history.
Product regression investigation follows integration commits with:

```bash
git log --first-parent main
git bisect start --first-parent
```

A branch-level investigation may encounter its documented intentional RED commits. Release tags,
when a release process is accepted, attach only to first-parent `main` commits.

After merge, delete the short-lived `feat/` branch unless the owner needs it temporarily for an
explicit follow-up.

## 9. Phase and Milestone gates

Each implementation Phase maps to one GitHub Milestone. Every Milestone contains one `[Gate]` Issue
with Entry gate, Exit gate, and Evidence sections. The Gate Issue closes last, followed by the
Milestone.

Design work, spikes, and fake-driven prototypes may proceed before an Entry gate closes.
Contract-owning production implementation must not merge until its gate permits it.

When a Milestone is assembled or materially re-scoped, perform one horizontal design review before
the affected implementation begins. Record the review in the Phase's Gate Issue using its required
checklist:

1. **Shared mechanisms**: inventory tokens, identifiers, results, queues, ordering, retry, and other
   mechanisms used across Issues. Assign one existing or new design authority to every unresolved
   mechanism shared by two or more Issues.
2. **Contract surfaces**: list every new or changed wire surface, persistence schema, public API, and
   stable identifier. Register every missing surface as Planned before implementation.
3. **Dependencies and intake**: state which shared substrate each dependency represents. When a new
   downstream consumer changes the Phase, re-read and disposition affected backlog Issues.
4. **Planned sections**: identify every non-normative outline and verify its authority banner.

The review completes only after every finding has a disposition and owner, the evidence is linked,
and the repository owner accepts the result. A material re-scope repeats the review for the changed
scope and pauses only the affected work.

An unresolved specification section opens with this banner:

> **Planned, not yet normative:** Issue/ADR #NN owns this mechanism. Implementers must not treat
> this outline as a finalized contract.

See [the Issue breakdown](07_issue_breakdown.md).

## 10. Instructions for coding agents

A coding-agent assignment provides:

1. the complete Issue text and frozen acceptance checklist when applicable;
2. applicable reference-document sections;
3. related ADRs and Contract Registry rows;
4. an instruction to follow Sections 1 through 9 of this workflow;
5. an instruction to confirm RED before production implementation;
6. an instruction to copy the acceptance checklist into the PR;
7. the current limit of the agent's authority, including whether merge has been explicitly delegated.

The root [`AGENTS.md`](../AGENTS.md) is a short mandatory entry point. This document remains the
complete authority; do not duplicate it in agent-specific files.

## 11. Staged CI requirements

Phase 0A PRs require Linux and Windows configure/build/test, clang-format, and core dependency
isolation. CI reruns on every merge commit pushed to `main` to verify the green-main policy.

Before the Phase 0A Gate closes, add clang-tidy, Linux ASan/UBSan, documentation/link validation,
and secret scanning. Later Gate Issues own TSan, process-containment tests, fault injection, Sitos
integration, and GPU/hardware qualification. These are owner-verified merge preconditions rather
than Ruleset-required status checks. This avoids blocking owner recovery on renamed or unavailable
CI jobs. Do not enable checks for components before their owning Phase.

## 12. Release flow

> **Planned, not yet normative:** The Phase 6 release Design Issue and ADR own this mechanism.
> Implementers must not treat this outline as a finalized contract.

`main` remains green, but Sitometron creates no formal release tag before the Phase 6 Gate completes.
Phase 6 decides packaging units, version coordination, release tooling, signing, and publication.
The bootstrap does not adopt release-please or another release automation tool.
