# Coding-agent instructions

Read and follow [the complete development workflow](docs/development_workflow.md) before changing
this repository.

Non-negotiable rules:

- No Ticket, No Commit.
- Work only on `feat/<issue-number>-<short-kebab-description>` for the assigned Issue.
- Read the complete Issue, referenced ADRs, and affected Contract Registry rows.
- Do not expand the frozen Issue scope or make drive-by fixes.
- Confirm and record structured RED evidence before production implementation.
- Keep `sitometron_core` source, manifest, and CMake integration standard-library-only until Issue #17 is owner-authorized, implemented, and merged. After that merge, use only reviewed standard headers and ADR-0004-approved direct dependency targets/includes, and expose no dependency-owned public types.
- Do not copy proprietary code, schemas, protocols, identifiers, or internal structures.
- Never push directly to `main`.
- Never enable auto-merge.
- Never merge a PR unless the owner explicitly instructs you to merge that PR.
- Passing CI or review does not imply merge authorization.

When work is merge-ready, report evidence and residual risks, then wait for the owner's decision.
