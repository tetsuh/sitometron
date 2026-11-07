"""ADR-0012 tooling acceptance and mutation controls (no runtime dependency)."""
import copy
import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

from jsonschema.exceptions import ValidationError

from tools.json_schema_validator import LocalDraft202012Schemas

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "validate_core_contract", ROOT / "tools" / "validate_core_contract.py"
)
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class SupervisorContractTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="issue133-supervisor-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.schemas = self.root / "schemas" / "core" / "v1"
        shutil.copytree(ROOT / "schemas" / "core" / "v1", self.schemas)
        self.path = self.schemas / "supervisor-decision-contract.json"
        self.document = json.loads(self.path.read_text(encoding="utf-8"))

    def save(self):
        self.path.write_text(json.dumps(self.document), encoding="utf-8")

    def check(self, name):
        function = getattr(VALIDATOR, name, None)
        self.assertTrue(callable(function), f"missing ADR-0012 validator check: {name}")
        return function(self.root)

    def rejected(self, name):
        # Missing implementation is RED, never a successful mutation rejection.
        self.assertTrue(callable(getattr(VALIDATOR, name, None)),
                        f"missing ADR-0012 validator check: {name}")
        with self.assertRaises((AssertionError, ValueError, ValidationError),
                               msg="mutation was silently accepted"):
            self.check(name)

    def test_schema_and_all_snapshot_representatives(self):
        registry = LocalDraft202012Schemas.load(self.schemas)
        registry.validate_instance(self.schemas / "supervisor-decision-contract.schema.json",
                                   self.document)
        self.assertEqual(25, len(self.document["seeds"]))

    def test_vectors_follow_rules(self):
        self.check("validate_supervisor_vectors")

    def test_reducer_fixed_point_is_complete(self):
        self.check("validate_supervisor_complete")

    def test_wrong_expected_rule_is_rejected(self):
        self.document["vectors"][0]["expected"]["rule"] = 2
        self.save()
        self.rejected("validate_supervisor_vectors")

    def test_missing_reachable_class_is_rejected(self):
        self.document["classes"].pop()
        self.save()
        self.rejected("validate_supervisor_complete")

    def test_missing_presence_vector_is_rejected(self):
        self.document["vectors"].pop()
        self.save()
        self.rejected("validate_supervisor_complete")

    def test_duplicate_vector_is_rejected(self):
        self.document["vectors"].append(copy.deepcopy(self.document["vectors"][0]))
        self.save()
        self.rejected("validate_supervisor_vectors")

    def test_wrong_wait_sources_are_rejected(self):
        vector = next(v for v in self.document["vectors"] if v["expected"]["kind"] == "Wait")
        vector["expected"]["sources"] = []
        self.save()
        self.rejected("validate_supervisor_vectors")

    def test_forbidden_producer_is_rejected(self):
        self.document["vectors"][0]["expected"].update(kind="Submit", event="worker_running")
        self.save()
        self.rejected("validate_supervisor_vectors")

    def test_first_match_priority_is_not_optional(self):
        snapshot_ids = {c["id"] for c in self.document["classes"]
                        if c["snapshot"]["state"] == "finalizing"
                        and c["snapshot"]["session_retention_status"] == "not_started"
                        and c["snapshot"]["finalization_status"] == "failed"}
        vector = next(v for v in self.document["vectors"] if v["class_id"] in snapshot_ids)
        vector["expected"].update(rule=9, event="terminal_outcome_committed",
                                  payload_rule="latched_reason_or_success_candidate")
        self.save()
        self.rejected("validate_supervisor_vectors")

    def test_seed_25_cannot_be_replaced_by_duplicate_general_case(self):
        self.document["seeds"][-1] = copy.deepcopy(self.document["seeds"][-2])
        self.document["seeds"][-1]["id"] = "S25"
        self.save()
        self.rejected("validate_supervisor_complete")

    def test_undeliverable_edges_are_never_silently_skipped(self):
        self.document["reducer_accepted_but_undeliverable"].pop()
        self.save()
        self.rejected("validate_supervisor_complete")

    def test_bad_seed_identity_binding_is_rejected(self):
        seed = self.document["seeds"][0]
        after = next(c["snapshot"] for c in self.document["classes"]
                     if c["id"] == seed["after_class"])
        after["session_id"] = "01890f30-7b54-7cc3-98c4-dc0c0c073999"
        self.save()
        self.rejected("validate_supervisor_complete")

    def test_unsupported_reducer_ast_is_not_ignored(self):
        path = self.schemas / "job-reducer-contract.json"
        reducer = json.loads(path.read_text(encoding="utf-8"))
        reducer["event_matrix"][0]["cases"][0]["when"]["operator"] = "unknown"
        path.write_text(json.dumps(reducer), encoding="utf-8")
        self.rejected("validate_supervisor_complete")

    def test_assigned_submit_must_be_accepted_by_reducer(self):
        path = self.schemas / "job-reducer-contract.json"
        reducer = json.loads(path.read_text(encoding="utf-8"))
        row = next(r for r in reducer["event_matrix"] if r["pair_id"] == "admitted__resources_committed")
        row["cases"][0] = {"case_id": "reject_all", "when": {"operator": "always"},
                           "disposition": "reject", "rejection_code": "event_not_allowed_in_state"}
        path.write_text(json.dumps(reducer), encoding="utf-8")
        self.rejected("validate_supervisor_complete")


if __name__ == "__main__":
    unittest.main()
