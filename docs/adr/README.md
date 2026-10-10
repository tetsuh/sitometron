# Architecture Decision Records

ADRs use four-digit sequence numbers and lowercase hyphenated names.

| ADR | Status | Decision |
|---|---|---|
| [0001](0001-bootstrap-a-stdlib-only-cpp20-core.md) | Superseded | Bootstrap a standard-library-only C++20 core |
| [0002](0002-define-core-job-reducer-contract.md) | Accepted | Define the core Job reducer contract |
| [0003](0003-define-single-state-writer-ingress-contract.md) | Accepted | Define the single-state-writer ingress contract |
| [0004](0004-allow-explicit-core-dependencies.md) | Accepted | Allow explicit dependencies in the C++20 core |
| [0005](0005-define-phase-0a-core-capability-port-contracts.md) | Accepted | Define Phase 0A core capability port contracts |
| [0006](0006-define-physical-jobjournal-durability-contract.md) | Accepted | Define the physical JobJournal durability contract |
| [0007](0007-leave-pre-existing-journal-paths-to-the-operator.md) | Accepted | Leave pre-existing Journal paths to the operator |
| [0008](0008-define-the-external-rest-v1-job-surface.md) | Accepted | Define the External REST v1 Job surface |
| [0009](0009-settle-the-external-rest-v1-gaps-found-by-the-prototype.md) | Accepted | Settle the External REST v1 gaps found by the prototype |
| [0010](0010-use-boost-beast-and-asio-for-the-external-rest-v1-adapter.md) | Accepted | Use Boost.Beast and Boost.Asio for the External REST v1 adapter |
| [0011](0011-use-nlohmann-json-for-the-external-rest-v1-adapter.md) | Proposed | Use nlohmann-json for the External REST v1 adapter |

Use [the ADR template](template.md). See [the ADR process](../10_adr_process.md) and
[the development workflow](../development_workflow.md) for status, review, and merge rules.
