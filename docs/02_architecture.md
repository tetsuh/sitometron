# Sitometron architecture

## 1. Dependency direction

Sitometron uses ports and adapters. Domain policy points outward only through Sitometron-owned port
interfaces and domain types.

```text
sitometrond
  -> adapters
      -> sitometron_core ports
  -> sitometron_core
```

Adapters may depend on the core. The core must not depend on adapters, unapproved third-party
libraries, third-party frameworks, or platform APIs. Accepted
[ADR-0004](adr/0004-allow-explicit-core-dependencies.md) replaces ADR-0001's standard-library-only
restriction with a closed direct allowlist while preserving this dependency direction and all
I/O/framework ownership. Only `sitometrond` composes concrete adapters.

## 2. Initial targets

| Target | Responsibility | Dependency policy |
|---|---|---|
| `sitometron_core` | Domain state, commands, pure reducer, lifecycle ports, and private single-writer orchestration | Implemented ADR-0004/`NFR-005` closed allowlist; Sitometron-owned public types only |
| `sitometron_test_support` and private fake-support targets | Deterministic fakes, barriers, and test helpers | Tests only |
| `sitometrond` | Composition root and daemon entry point | Core initially; adapters by Phase |

Later adapter targets are introduced only by their owning Issues:

- `sitometron_journal`;
- `sitometron_http`;
- `sitometron_process`;
- `sitometron_topology`;
- `sitometron_sitos`;
- `sitometron_logging_quill`.

The C++ and Python Worker SDKs remain language-specific edges around a language-independent Worker
protocol.

## 3. Runtime ownership

The core owns deterministic policy. Adapters own I/O and external side effects. A single Job-state
writer orders accepted commands and events. Journal and external-side-effect commit protocols are
specified before production adapters use them.

Accepted ADR-0002 owns the normative Job state, event, command, rejection, reducer, and logical
JobJournal mechanism. Issue #9 implements the dependency-minimal pure reducer, and Issue #22
preserves its behavior while reducing structural complexity. Accepted ADR-0005 and Issue #11
implement the lifecycle capability ports and deterministic fakes. Accepted ADR-0003 and Issue #12
implement the bounded private single writer, complete logical envelope construction, commit ordering,
and no-fail postcommit dispatch; Issue #13 completes deterministic adverse/race qualification.
Physical Journal durability and production effect adapters remain Planned under later owners.

The machine-readable Job transition contract is documented in
[the core contracts](03_core_contracts.md). Worker HTTP schemas, external REST schemas, physical
Journal records, containment markers, and Artifact manifests remain separate contract surfaces in
the [Contract Registry](08_contract_registry.md).

## 4. Application boundary

Sitometron launches only deployment-registered trusted Applications. Clients do not submit commands,
modules, executable paths, or scripts. An Application receives opaque Job, Session, resource, and
control context through the runner boundary and reports lifecycle events. Image, tensor, and raw
input payloads never traverse Worker control.

## 5. Persistence boundary

JobJournal records control-plane facts and reducer authority. Operational logs do not reconstruct
Job state. Sitos owns its own Session and retained-buffer persistence. An adapter coordinates these
authorities without merging their schemas or replay semantics.

## 6. Platform boundary

Linux and Windows share domain policy and protocol contracts. Process containment, affinity,
filesystem synchronization, and platform diagnostics remain adapter implementations qualified on
each platform.

See [the detailed boundary rules](architecture/boundaries.md), superseded
[ADR-0001](adr/0001-bootstrap-a-stdlib-only-cpp20-core.md), and Accepted
[ADR-0004](adr/0004-allow-explicit-core-dependencies.md).
