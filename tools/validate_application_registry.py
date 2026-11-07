"""Validate the Application registry's shared stage-A instances offline (ADR-0015)."""

from __future__ import annotations

import json
from pathlib import Path

from jsonschema.exceptions import ValidationError

from json_schema_validator import LocalDraft202012Schemas


def _inventories(vectors: dict) -> tuple[list, list]:
    if vectors.get("format") != "application-registry-vectors.v1" or vectors.get("authority") != "ADR-0015":
        raise ValueError("registry vector format/authority is invalid")
    schema_vectors = vectors["schema_vectors"]
    loader_vectors = vectors["loader_only_vectors"]
    if not isinstance(schema_vectors, list) or len(schema_vectors) < 179:
        raise ValueError("registry schema vector inventory is incomplete (minimum 179)")
    if not isinstance(loader_vectors, list) or len(loader_vectors) < 69:
        raise ValueError("registry loader-only vector inventory is incomplete (minimum 69)")
    return schema_vectors, loader_vectors


def _check_ids(schema_vectors: list, loader_vectors: list) -> None:
    ids: set[str] = set()
    for vector in [*schema_vectors, *loader_vectors]:
        identifier = vector["id"]
        if not isinstance(identifier, str) or not identifier or identifier in ids:
            raise ValueError("registry vector ID is missing or duplicated")
        ids.add(identifier)


def _check_expectations(schemas: LocalDraft202012Schemas, root: Path, schema_vectors: list) -> set[str]:
    executed: set[str] = set()
    for vector in schema_vectors:
        expected = vector["valid"]
        if type(expected) is not bool:
            raise ValueError("registry vector expectation must be boolean")
        try:
            schemas.validate_instance(root / "application-registry.schema.json", vector["instance"])
            accepted = True
        except ValidationError:
            accepted = False
        if accepted != expected:
            raise AssertionError(f"registry_schema_vectors [{vector['id']}]: expected {expected}, got {accepted}")
        executed.add(vector["id"])
    return executed


def validate_application_registry(project_root: Path) -> None:
    """Check the actual schema, vector inventory, and every positive/negative expectation."""
    root = project_root.resolve(strict=True) / "schemas" / "application-registry" / "v1"
    schemas = LocalDraft202012Schemas.load(root)
    vectors = json.loads((root / "application-registry-vectors.json").read_text(encoding="utf-8"))
    schema_vectors, loader_vectors = _inventories(vectors)
    _check_ids(schema_vectors, loader_vectors)
    executed = _check_expectations(schemas, root, schema_vectors)
    if len(executed) != len(schema_vectors):
        raise AssertionError("registry schema vectors were not consumed exactly once")
    print(f"registry_schema_vectors: schema instances executed={len(executed)} failures=0")
    print(f"registry loader-only rows={len(loader_vectors)} (owned rules/bytes/files; not JSON Schema)")


if __name__ == "__main__":
    validate_application_registry(Path(__file__).resolve().parents[1])
