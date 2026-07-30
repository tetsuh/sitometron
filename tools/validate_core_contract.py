"""Validate the proposed core contract with the pinned Draft 2020-12 tooling."""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

from jsonschema.exceptions import ValidationError

from json_schema_validator import LocalDraft202012Schemas

_INSTANCE_PAIRS = (
    ("job-reducer-contract.schema.json", "job-reducer-contract.json"),
    ("job-reducer-vectors.schema.json", "job-reducer-vectors.json"),
)


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _require_reject_normalization_forbidden(
    schemas: LocalDraft202012Schemas,
    schema_root: Path,
    contract: dict[str, Any],
) -> None:
    mutated = copy.deepcopy(contract)
    reject_case = next(
        case
        for matrix_name in ("command_matrix", "event_matrix")
        for row in mutated[matrix_name]
        for case in row["cases"]
        if case["disposition"] == "reject"
    )
    reject_case["normalize_to"] = "late_worker_event"
    try:
        schemas.validate_instance(schema_root / "job-reducer-contract.schema.json", mutated)
    except ValidationError:
        print("Validated that rejected inputs cannot request normalization")
        return
    raise AssertionError("reject cases must forbid normalize_to")


def validate_core_contract(project_root: Path) -> None:
    """Validate core schemas, local references, formats, and contract instances."""
    schema_root = project_root.resolve(strict=True) / "schemas" / "core" / "v1"
    schemas = LocalDraft202012Schemas.load(schema_root)
    instances = {
        instance_name: _load_json(schema_root / instance_name)
        for _, instance_name in _INSTANCE_PAIRS
    }

    for schema_name, instance_name in _INSTANCE_PAIRS:
        schemas.validate_instance(schema_root / schema_name, instances[instance_name])
        print(f"Validated {instance_name} against {schema_name}")

    _require_reject_normalization_forbidden(
        schemas,
        schema_root,
        instances["job-reducer-contract.json"],
    )
    print(f"Validated {len(schemas.schemas)} Draft 2020-12 core schemas")


if __name__ == "__main__":
    validate_core_contract(Path(__file__).resolve().parents[1])
