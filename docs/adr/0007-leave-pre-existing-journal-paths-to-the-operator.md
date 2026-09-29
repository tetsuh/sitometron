# ADR-0007: Leave pre-existing Journal paths to the operator

## Status

Proposed — 2026-09-29

## Context

ADR-0006 Section 2 states that Journal access is owner-only and fixes how the adapter creates the
Journal directory (`0700`) and segment files (`0600`) on POSIX and an owner-only access control list
on Windows. Section 3 makes each new segment's file and directory entry durable, and the segment
writer (Issue #59) also syncs the parent of every directory it creates.

ADR-0006 does not say what startup does when these paths already exist:

- a Journal directory or segment whose permissions are broader than owner-only, for example created
  by the operator or restored from a backup;
- ancestor directories left by an earlier failed creation, whose directory entries may not be
  durable.

Review of PR #60 raised both cases. The owner deferred them to Issue #62, which compared three
options: inspect and refuse, repair silently, or document the boundary. v0.1 listens on loopback
only, runs under one operator account, and qualifies Windows behavior in Phase 6. Checking an
existing Windows access control list for "no grant beyond the owner" is most of the cost of the
inspecting option.

## Decision

We will treat ADR-0006's owner-only rule and new-path durability rule as creation-time guarantees
only.

- The daemon applies owner-only permissions to every Journal directory and segment it creates, as
  ADR-0006 Section 2 states.
- The daemon does not inspect, tighten, or refuse the permissions of a Journal directory or segment
  that already exists. Keeping pre-existing Journal paths owner-only is the operator's
  responsibility.
- The daemon does not re-prove the durability of ancestor directories that already exist, including
  ones left by an earlier failed creation. Provisioning the parent path of the Journal directory is
  the operator's responsibility. The Journal directory's own entry is still synced into its parent on
  every open, as the segment writer already does.

This ADR adds no startup refusal code and does not change `OPS-002`.

## Consequences

- Good: no new startup refusal, no platform-specific permission inspection, and no change to
  operator-owned configuration.
- Good: the scope of ADR-0006 Section 2 is explicit, so reviewers and implementers no longer read it
  as a runtime invariant on existing paths.
- Bad: a Journal directory or segment made readable by other local users is not detected. Its records
  can include Job arguments and paths.
- Bad: a parent path left non-durable by an earlier failure is not repaired by the daemon.
- Neutral: Phase 6 packaging and qualification may revisit this with an inspecting check on both
  platforms through a superseding ADR.

## Options considered

- **Inspect and refuse at startup (POSIX mode and Windows access control list)**: rejected for v0.1
  because the Windows inspection is the larger part of the work and the loopback-only, single-operator
  deployment gains little from it now. It remains the preferred direction if Phase 6 decides that
  startup must enforce owner-only access.
- **Repair silently**: rejected because it changes operator-owned configuration without notice.
- **Inspect on POSIX only**: rejected because the contract would then differ by platform.

## References

- Issue #62
- PR #60 (review findings on pre-existing paths and ancestor durability)
- Requirements: `JRN-007`, `OPS-002` (unchanged)
- Contract Registry: `Physical JobJournal record encoding and segment layout`
- Related ADR: ADR-0006
