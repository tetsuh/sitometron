# Schemas

Machine-readable contracts are registered here before implementation.

- `core/v1/`: normative ADR-0002 raw-candidate and internal reducer-input DTOs, Job states,
  commands, Journal events, exhaustive reducer matrix, vectors, and checked state diagram;
- `openapi/`: External REST contracts; `rest-v1.openapi.json` describes External REST v1 under
  Accepted ADR-0008 and ADR-0009, which remain the authority (Issue #88);
- `application-registry/v1/`: normative Phase 2 trusted application records under Accepted
  [ADR-0015](../docs/adr/0015-register-applications-with-a-verified-registry-file.md). Issue #134
  implements the registry-file portion; launch identity remains with Issue #133.
  `application-registry.schema.json` states field rules and annotates file-level
  rules; `application-registry-vectors.json` is shared by the offline JSON Schema validator and
  the C++ loader's private stage A (schema rules only). Unique identifiers and native absolute
  paths are owned stage B checks; byte rules, readable regular files after following symbolic
  links, and SHA-256 verification are loader checks, not JSON Schema assertions;
- `worker-protocol/`: Worker loopback HTTP JSON schemas.

A contract becomes normative only when its named authority is Accepted and its Contract Registry row
is Normative. Each contract carries an explicit status and authority. Core schemas are validated
generically by `tools/validate_core_contract.py` and semantically by their contract-specific checker.
