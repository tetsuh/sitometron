import json
import tempfile
import unittest
from pathlib import Path

from jsonschema.exceptions import SchemaError, ValidationError
from referencing.exceptions import Unresolvable

from tools.json_schema_validator import LocalDraft202012Schemas


class LocalDraft202012SchemasTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write_json(self, name: str, value: object) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_accepts_valid_instance_with_local_relative_reference(self) -> None:
        self.write_json(
            "identifier.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "identifier.schema.json",
                "$defs": {"identifier": {"type": "integer", "minimum": 1}},
            },
        )
        root_schema = self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "type": "object",
                "additionalProperties": False,
                "required": ["identifier"],
                "properties": {
                    "identifier": {
                        "$ref": "identifier.schema.json#/$defs/identifier"
                    }
                },
            },
        )
        schemas = LocalDraft202012Schemas.load(self.root)

        schemas.validate_instance(root_schema, {"identifier": 7})

    def test_accepts_local_reference_under_nested_identifier(self) -> None:
        self.write_json(
            "sub/target.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "sub/target.schema.json",
                "type": "integer",
                "minimum": 1,
            },
        )
        root_schema = self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "properties": {
                    "value": {"$id": "sub/", "$ref": "target.schema.json"}
                },
            },
        )
        schemas = LocalDraft202012Schemas.load(self.root)

        schemas.validate_instance(root_schema, {"value": 7})

    def test_rejects_invalid_schema(self) -> None:
        self.write_json(
            "invalid.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "invalid.schema.json",
                "type": 42,
            },
        )

        with self.assertRaises(SchemaError):
            LocalDraft202012Schemas.load(self.root)

    def test_rejects_relative_reference_under_nested_remote_identifier(self) -> None:
        self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "properties": {
                    "value": {
                        "$id": "https://example.invalid/sub/",
                        "$ref": "target.schema.json",
                    }
                },
            },
        )

        with self.assertRaises(Unresolvable):
            LocalDraft202012Schemas.load(self.root)

    def test_ignores_reference_shaped_data_inside_const(self) -> None:
        value = {"$ref": "https://example.invalid/not-a-schema-reference"}
        root_schema = self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "const": value,
            },
        )
        schemas = LocalDraft202012Schemas.load(self.root)

        schemas.validate_instance(root_schema, value)

    def test_rejects_unresolved_local_reference(self) -> None:
        self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "$ref": "missing.schema.json",
            },
        )

        with self.assertRaises(Unresolvable):
            LocalDraft202012Schemas.load(self.root)

    def test_rejects_remote_reference(self) -> None:
        self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "$ref": "https://example.invalid/remote.schema.json",
            },
        )

        with self.assertRaises(Unresolvable):
            LocalDraft202012Schemas.load(self.root)

    def test_rejects_invalid_instance(self) -> None:
        root_schema = self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "type": "integer",
                "minimum": 1,
            },
        )
        schemas = LocalDraft202012Schemas.load(self.root)

        with self.assertRaises(ValidationError):
            schemas.validate_instance(root_schema, 0)

    def test_enforces_known_format_annotations(self) -> None:
        root_schema = self.write_json(
            "root.schema.json",
            {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "$id": "root.schema.json",
                "type": "string",
                "format": "date-time",
            },
        )
        schemas = LocalDraft202012Schemas.load(self.root)

        with self.assertRaises(ValidationError):
            schemas.validate_instance(root_schema, "not-a-date-time")


if __name__ == "__main__":
    unittest.main()
