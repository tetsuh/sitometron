"""Validate the proposed core contract with the pinned Draft 2020-12 tooling."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from json_schema_validator import LocalDraft202012Schemas

_INSTANCE_PAIRS = (
    ("job-reducer-contract.schema.json", "job-reducer-contract.json"),
    ("job-reducer-vectors.schema.json", "job-reducer-vectors.json"),
)


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def validate_core_contract(project_root: Path) -> None:
    """Validate core schemas, local references, formats, and contract instances."""
    schema_root = project_root.resolve(strict=True) / "schemas" / "core" / "v1"
    schemas = LocalDraft202012Schemas.load(schema_root)

    for schema_name, instance_name in _INSTANCE_PAIRS:
        schemas.validate_instance(
            schema_root / schema_name,
            _load_json(schema_root / instance_name),
        )
        print(f"Validated {instance_name} against {schema_name}")

    print(f"Validated {len(schemas.schemas)} Draft 2020-12 core schemas")


if __name__ == "__main__":
    validate_core_contract(Path(__file__).resolve().parents[1])
