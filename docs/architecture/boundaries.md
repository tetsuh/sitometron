# Architecture boundaries

## Core

`sitometron_core` owns domain identifiers, commands, events, reducers, lifecycle rules, admission,
and scheduling policy. It must remain deterministic and currently depends only on the C++ standard
library under ADR-0001.

> **Proposed, not yet normative:** [ADR-0004](../adr/0004-allow-explicit-core-dependencies.md)
> would preserve the reviewed standard-header allowlist and permit only nlohmann/json, Boost.UUID,
> and Boost.Hash2 behind Sitometron-owned public types. The adapter ownership below does not change,
> and ADR-0001 remains effective until ADR-0004 is owner-accepted.

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
