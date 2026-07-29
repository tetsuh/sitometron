# Architecture boundaries

## Core

`sitometron_core` owns domain identifiers, commands, events, reducers, lifecycle rules, admission,
and scheduling policy. It must remain deterministic and depend only on the C++ standard library.

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
