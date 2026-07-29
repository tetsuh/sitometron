# Architecture Decision Record process

## 1. Purpose

An Architecture Decision Record preserves why a consequential choice was made, which alternatives
were rejected, and which contracts and implementations it governs.

## 2. Placement and naming

```text
docs/adr/
  README.md
  template.md
  NNNN-short-kebab-case-title.md
```

Use a four-digit, zero-padded sequence. Gaps are allowed. Rejected proposals retain their number.
Each ADR owns one decision; independent decisions use separate ADRs.

## 3. Required structure

Use [the template](adr/template.md) with:

- Status and decision date;
- Context;
- Decision;
- Consequences;
- Options considered;
- References.

## 4. Status transitions

```text
Proposed -> Accepted -> Deprecated
             |
             +-> Superseded by ADR-NNNN
Proposed -> Rejected
```

- **Proposed**: open for review and not implementation authority.
- **Accepted**: approved by the owner and merged as an effective decision.
- **Rejected**: reviewed and not adopted; retained with its reasons.
- **Superseded**: replaced by a named later ADR.
- **Deprecated**: the governed capability is no longer supported.

After an ADR becomes Accepted, do not rewrite Context, Decision, Consequences, or rejected options.
Use a superseding ADR to change the decision. Typo, formatting, and broken-link corrections are
allowed when they do not change meaning.

## 5. When an ADR is required

An ADR is required for the classes listed in
[the development workflow](development_workflow.md#5-architecture-decisions), including lifecycle,
wire, public API, stable identifier, ordering, durability, persistence-authority, security,
platform, build-foundation, and overlapping-contract decisions.

An ADR is not normally required for a behavior-preserving refactor, test-only change, typo, or
mechanical documentation correction.

## 6. Review and merge flow

1. Create the design Issue and register Planned Contract Registry rows.
2. Add a Proposed ADR and review it against the Issue, Requirements, and Registry.
3. Record findings and owner decisions.
4. Change the status to Accepted with its date only after owner approval.
5. Merge according to the owner-only merge policy.
6. Begin or merge production implementation only when the Accepted decision and Phase Gate permit it.

ADRs normally use a separate PR from production implementation. A small decision may be bundled with
its implementation PR when both share one Issue scope and remain independently reviewable. Include
`[ADR]` in the bundled PR title. The owner determines whether the exception is appropriate.

Independent secondary review is optional and owner-directed. Automated review or CI never accepts
an ADR on the owner's behalf.

## 7. Contract Registry integration

A Proposed ADR names every affected Contract Registry row and intended maturity transition. The
Accepted ADR becomes the single normative authority for its decision. Implementation status changes
only when implementation evidence merges; ADR acceptance alone does not mark code Implemented.

A new unresolved mechanism shared by multiple Issues uses one existing or new design authority.
Do not create overlapping ADRs for the same decision.
