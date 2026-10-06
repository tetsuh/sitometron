# Schemas

Machine-readable contracts are registered here before implementation.

- `core/v1/`: normative ADR-0002 raw-candidate and internal reducer-input DTOs, Job states,
  commands, Journal events, exhaustive reducer matrix, vectors, and checked state diagram;
- `openapi/`: External REST contracts; `rest-v1.openapi.json` describes External REST v1 under
  Accepted ADR-0008 and ADR-0009, which remain the authority (Issue #88);
- `application-registry/`: trusted application records;
- `worker-protocol/`: Worker loopback HTTP JSON schemas.

A contract becomes normative only when its named authority is Accepted and its Contract Registry row
is Normative. Each contract carries an explicit status and authority. Core schemas are validated
generically by `tools/validate_core_contract.py` and semantically by their contract-specific checker.
