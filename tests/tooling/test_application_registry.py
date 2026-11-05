"""The schema half of APP-001 uses the same instances as the C++ stage-A check."""

import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "tools" / "validate_application_registry.py"
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
RELATIVE = Path("schemas/application-registry/v1")


class ApplicationRegistrySchemaTest(unittest.TestCase):
    def helper(self):
        self.assertTrue(HELPER.is_file(), "registry schema validator is not implemented")
        spec = importlib.util.spec_from_file_location("validate_application_registry", HELPER)
        self.assertIsNotNone(spec)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def fixture(self, directory):
        target = Path(directory) / RELATIVE
        shutil.copytree(ROOT / RELATIVE, target)
        return Path(directory), target

    def test_registry_schema_vectors(self):
        self.helper().validate_application_registry(ROOT)

    def test_rejects_wrong_positive_expectation(self):
        helper = self.helper()
        with tempfile.TemporaryDirectory(prefix="issue134-schema-") as directory:
            root, target = self.fixture(directory)
            path = target / "application-registry-vectors.json"
            vectors = json.loads(path.read_text(encoding="utf-8"))
            next(v for v in vectors["schema_vectors"] if v["valid"])["valid"] = False
            path.write_text(json.dumps(vectors), encoding="utf-8")
            with self.assertRaises((AssertionError, ValueError)):
                helper.validate_application_registry(root)

    def test_rejects_wrong_negative_expectation(self):
        helper = self.helper()
        with tempfile.TemporaryDirectory(prefix="issue134-schema-") as directory:
            root, target = self.fixture(directory)
            path = target / "application-registry-vectors.json"
            vectors = json.loads(path.read_text(encoding="utf-8"))
            next(v for v in vectors["schema_vectors"] if not v["valid"])["valid"] = True
            path.write_text(json.dumps(vectors), encoding="utf-8")
            with self.assertRaises((AssertionError, ValueError)):
                helper.validate_application_registry(root)

    def test_rejects_duplicate_vector_ids(self):
        helper = self.helper()
        with tempfile.TemporaryDirectory(prefix="issue134-schema-") as directory:
            root, target = self.fixture(directory)
            path = target / "application-registry-vectors.json"
            vectors = json.loads(path.read_text(encoding="utf-8"))
            vectors["schema_vectors"].append(vectors["schema_vectors"][0])
            path.write_text(json.dumps(vectors), encoding="utf-8")
            with self.assertRaises((AssertionError, ValueError)):
                helper.validate_application_registry(root)

    def test_rejects_empty_vector_set(self):
        helper = self.helper()
        with tempfile.TemporaryDirectory(prefix="issue134-schema-") as directory:
            root, target = self.fixture(directory)
            path = target / "application-registry-vectors.json"
            vectors = json.loads(path.read_text(encoding="utf-8"))
            vectors["schema_vectors"] = []
            path.write_text(json.dumps(vectors), encoding="utf-8")
            with self.assertRaises((AssertionError, ValueError)):
                helper.validate_application_registry(root)


if __name__ == "__main__":
    unittest.main()
