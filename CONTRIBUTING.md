# Contributing

Sitometron uses English for code, comments, Issues, pull requests, commits, schemas, and normative
documentation.

Before changing the repository:

1. read [the clean-room policy](docs/clean-room-policy.md) and
   [the development workflow](docs/development_workflow.md);
2. create or select the owning Issue and read it completely;
3. read its referenced ADRs and affected Contract Registry rows, then identify its Phase, Gate,
   Requirement IDs, dependencies, and remaining applicable artifacts;
4. create `feat/<issue-number>-<short-kebab-description>` from current `main`;
5. follow RED, GREEN, REFACTOR for production behavior;
6. open one PR with `Closes #NN` and the required evidence.

ADRs normally use a separate PR. A small decision may share an implementation PR when the owner
accepts the exception described in [the ADR process](docs/10_adr_process.md).

The owner makes every merge decision. A coding agent may merge only after explicit one-time
authorization for that PR at its current head. Normal merge is the default; the owner may choose
squash merge. Do not enable auto-merge or push directly to `main`.

Format C++ with the repository `.clang-format`. Project code is compiled with warnings as errors.
Tests must be deterministic; unit tests must not use real sleeps when fakes can express behavior.
