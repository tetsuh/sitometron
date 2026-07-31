# Architecture boundaries

## Core

`sitometron_core` owns domain identifiers, commands, events, reducers, lifecycle rules, admission,
and scheduling policy. It must remain deterministic and dependency-minimal under Accepted
[ADR-0004](../adr/0004-allow-explicit-core-dependencies.md). The reviewed standard-header allowlist
is preserved, and only nlohmann/json, Boost.UUID, and Boost.Hash2 are permitted behind
Sitometron-owned public types. The current implementation remains standard-library-only until
[dependency-integration Issue #17](https://github.com/tetsuh/sitometron/issues/17) activates the
Accepted boundary. The adapter ownership below does not change.

The core must not include:

- HTTP or authentication adapters;
- Sitos, Zenoh, or parameter interpretation;
- Holoscan, CUDA, Python, or application graph construction;
- logger backends or filesystem-specific persistence code;
- hwloc or operating-system process APIs.

## Adapters

Adapters implement narrow core ports:

| Target | Responsibility |
|---|---|
| `sitometron_journal` | Durable JobJournal implementation |
| `sitometron_http` | External and Worker loopback HTTP |
| `sitometron_process` | Linux and Windows process containment |
| `sitometron_topology` | Hardware discovery and affinity |
| `sitometron_sitos` | Session, parameter, and retained-buffer integration |
| `sitometron_logging_quill` | Operational logging and durable flush facilities |

Only `sitometrond` composes adapters.

## Application boundary

Sitometron starts only deployment-registered trusted applications. It passes opaque application,
session, resource, and control information. The application owns computation graphs, parameter
schemas, image processing, and artifact publication safe points.
