# Contributing

Sitometron uses English for code, comments, Issues, pull requests, commits, schemas, and normative
documentation.

Before contributing:

1. read the [clean-room policy](docs/clean-room-policy.md);
2. identify the owning Phase and Gate Issue;
3. identify affected Contract Registry rows or explain why none apply;
4. create an ADR before changing lifecycle, wire, public API, threading, durability, or persistence-authority contracts;
5. keep implementation and ADR pull requests separate.

Format C++ with the repository `.clang-format`. Project code is compiled with warnings as errors.
Tests must be deterministic; unit tests must not use real sleeps.
