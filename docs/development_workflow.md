# Development workflow

## Phase and Gate

Each implementation Phase maps to one GitHub Milestone. Every Milestone contains one `[Gate]`
Issue with Entry gate, Exit gate, and Evidence sections. The Gate Issue closes last; then the
Milestone closes.

Gate evidence consists of linked ADRs, Contract Registry rows, CI runs, and qualification reports.
Design work, spikes, and fake-driven prototypes may proceed before an Entry gate closes. Contract-
owning production implementation must not merge before its gate permits it.

## ADR requirement

An ADR is required before changing:

- lifecycle or reducer semantics;
- wire formats, routes, or public APIs;
- threading and ordering guarantees;
- durability or persistence authority;
- security and authentication boundaries.

ADRs and implementation changes use separate pull requests. States are Proposed, Accepted,
Superseded, and Deprecated.

## Contract registration

Register a Planned contract row before implementation. Contract maturity and implementation status
are independent. Every Issue and pull request identifies affected rows or gives an N/A rationale.

## Pull requests

A pull request records:

- Phase and Gate Issue;
- related Issue and ADR;
- affected Contract Registry rows or N/A;
- tests and evidence;
- clean-room provenance confirmation.
