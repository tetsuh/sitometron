# Clean-room development policy

Sitometron is a clean-room implementation developed from public requirements and independently
written design records.

## Prohibited inputs

Do not copy or adapt proprietary source code, schemas, wire protocols, identifiers, internal
structures, comments, tests, or documentation. Do not reproduce legacy product-specific behavior
unless a public Sitometron requirement independently defines it.

The general software terms `Controller`, `Calc`, and `Job` are permitted. A familiar term is not a
license to copy an implementation or protocol.

## Required practice

- Record architecture and contract decisions in Sitometron-owned ADRs and schemas.
- Explain requirements without citing proprietary implementation details.
- Keep public interfaces application-agnostic.
- Stop work and request review when provenance is uncertain.
- Preserve third-party license notices when required.

The repository starts without a NOTICE file. Add one only when an included dependency or artifact
requires attribution beyond the Apache-2.0 license text.
