import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPOSITORY_ROOT / "tools" / "check_repository_governance.py"

ADR_TEMPLATE = """# ADR-0009: Example decision

## Status

Accepted on 2026-08-28.

## Context

Context text.

## Decision

Decision text.

## Consequences

Consequences text.

## Options considered

Options text.

## References

- [the ADR process](../10_adr_process.md)
"""

REGISTRY_TEMPLATE = """# Contract Registry

Contract maturity values are `Planned`, `Normative`, `Deprecated`, and `Superseded`. Implementation
values are `Planned`, `In progress`, `Implemented`, and `Removed`.

## 2. Registered surfaces

| Contract surface | Maturity | Implementation | Normative or design authority | Owner |
|---|---|---|---|---|
| Implemented surface | Normative | Implemented | Accepted [ADR-0009](adr/0009-example.md) | Phase 0A |
| Normative but unbuilt surface | Normative | Planned | Accepted [ADR-0009](adr/0009-example.md) | Phase 0B |
| Future surface | Planned | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment | Phase 1 |
"""

BANNER = (
    "> **Planned, not yet normative:** [Issue #35](https://github.com/tetsuh/sitometron/issues/35)\n"
    "> tracks assignment of the future Design Issue and ADR that will own this mechanism.\n"
    "> Implementers must not treat this outline as a finalized contract.\n"
)


def load_validator() -> ModuleType:
    spec = importlib.util.spec_from_file_location("check_repository_governance", VALIDATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load the governance validator")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def issue_form(fields: list[tuple[str, str]], *, required: bool = True,
               checkbox_ids: tuple[str, ...] = ("provenance",)) -> str:
    lines = ["name: Example", "description: Example form", "body:"]
    for identifier, label in fields:
        if identifier in checkbox_ids:
            lines += [
                "  - type: checkboxes", f"    id: {identifier}", "    attributes:",
                f"      label: {label}", "      options:",
                "        - label: Example confirmation",
                f"          required: {'true' if required else 'false'}",
            ]
        else:
            lines += [
                "  - type: textarea", f"    id: {identifier}", "    attributes:",
                f"      label: {label}", "    validations:",
                f"      required: {'true' if required else 'false'}",
            ]
    return "\n".join(lines) + "\n"

class GovernancePresenceTest(unittest.TestCase):
    def test_validator_exists(self) -> None:
        self.assertTrue(VALIDATOR.is_file(), "tools/check_repository_governance.py is absent")

@unittest.skipUnless(VALIDATOR.is_file(), "governance validator is not implemented")
class IssueFormParserTest(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_validator()
    def test_parses_ids_labels_and_required_flags(self) -> None:
        text = issue_form([("summary", "Summary"), ("provenance", "Clean-room confirmation")])
        blocks = self.module.parse_issue_form(text)
        self.assertEqual(sorted(blocks), ["provenance", "summary"])
        self.assertEqual(blocks["summary"].label, "Summary")
        self.assertTrue(blocks["summary"].required)
        self.assertEqual(blocks["summary"].kind, "textarea")
        self.assertEqual(blocks["provenance"].kind, "checkboxes")
        self.assertTrue(blocks["provenance"].required)
    def test_rejects_duplicate_identifiers(self) -> None:
        text = issue_form([("summary", "Summary"), ("summary", "Duplicate")])
        with self.assertRaises(self.module.ValidationError):
            self.module.parse_issue_form(text)
    def test_reads_required_only_from_the_correct_block(self) -> None:
        misplaced = (
            "name: Example\nbody:\n"
            "  - type: textarea\n    id: summary\n    attributes:\n"
            "      label: Summary\n      required: true\n"
        )
        blocks = self.module.parse_issue_form(misplaced)
        self.assertFalse(blocks["summary"].required)
    def test_rejects_non_lf_or_non_utf8_forms(self) -> None:
        text = issue_form([("summary", "Summary")])
        with self.assertRaises(self.module.ValidationError):
            self.module.parse_issue_form(text.replace("\n", "\r\n"))
    def test_rejects_required_flags_in_the_wrong_kind_specific_block(self) -> None:
        textarea_options = ("name: Example\nbody:\n  - type: textarea\n    id: summary\n"
                            "    attributes:\n      label: Summary\n      options:\n      required: true\n")
        checkbox_validations = ("name: Example\nbody:\n  - type: checkboxes\n    id: provenance\n"
                                "    attributes:\n      label: Confirmation\n    validations:\n"
                                "          required: true\n")
        for text, identifier in ((textarea_options, "summary"), (checkbox_validations, "provenance")):
            self.assertFalse(self.module.parse_issue_form(text)[identifier].required)
    def test_rejects_a_block_without_an_identifier_or_label(self) -> None:
        for text in [
            issue_form([("valid", "Valid")])
            + "  - type: textarea\n    attributes:\n      label: Missing identifier\n",
            "name: Example\nbody:\n  - type: textarea\n    id: summary\n    attributes:\n",
        ]:
            with self.subTest(text=text):
                with self.assertRaises(self.module.ValidationError):
                    self.module.parse_issue_form(text)
    def test_allows_markdown_help_without_an_identifier(self) -> None:
        text = (
            "name: Example\nbody:\n"
            "  - type: markdown\n    attributes:\n      value: Helpful text\n"
            + issue_form([("summary", "Summary")]).partition("body:\n")[2]
        )
        blocks = self.module.parse_issue_form(text)
        self.assertEqual(list(blocks), ["summary"])

@unittest.skipUnless(VALIDATOR.is_file(), "governance validator is not implemented")
class GovernanceCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name).resolve()
        self.module = load_validator()
        self.write("docs/adr/0009-example.md", ADR_TEMPLATE)
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE)
        self.write("docs/10_adr_process.md", "# ADR process\n")
        self.write(".github/ISSUE_TEMPLATE/feature.yml", self.form(".github/ISSUE_TEMPLATE/feature.yml"))
        self.write(".github/ISSUE_TEMPLATE/adr.yml", self.form(".github/ISSUE_TEMPLATE/adr.yml"))
        self.write(".github/ISSUE_TEMPLATE/gate.yml", self.form(".github/ISSUE_TEMPLATE/gate.yml"))
        self.write(".github/pull_request_template.md",
                   "".join(f"- {field}:\n" for field in self.module.PULL_REQUEST_FIELDS))

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write(self, relative_path: str, text: str) -> None:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))

    def tracked(self) -> list[str]:
        return sorted(
            str(p.relative_to(self.root)).replace("\\", "/")
            for p in self.root.rglob("*") if p.is_file()
        )

    def check(self) -> list:
        return self.module.check_repository(self.root, self.tracked())

    def form(self, path: str, *, required: bool = True) -> str:
        contract = self.module.FORM_CONTRACTS[path]
        fields = [(identifier, label) for identifier, (label, _) in contract.items()]
        kinds = tuple(identifier for identifier, (_, kind) in contract.items() if kind == "checkboxes")
        text = issue_form(fields, required=required, checkbox_ids=kinds)
        text = text.replace("  - type: textarea\n    id: phase", "  - type: input\n    id: phase")
        return text.replace("  - type: textarea\n    id: adr\n", "  - type: dropdown\n    id: adr\n") if path.endswith("feature.yml") else text
    def test_accepts_a_conforming_repository(self) -> None:
        self.assertEqual(self.check(), [])
    def test_rejects_unknown_adr_status_values(self) -> None:
        for status in ["Draft", "accepted", "Approved", ""]:
            with self.subTest(status=status):
                self.write("docs/adr/0009-example.md",
                           ADR_TEMPLATE.replace("Accepted on 2026-08-28.", status or "\n"))
                self.assertTrue(any("status" in f.reason.lower() for f in self.check()))
    def test_requires_all_six_adr_metadata_sections_for_non_legacy_adrs(self) -> None:
        for section in self.module.ADR_SECTIONS:
            with self.subTest(section=section):
                self.write("docs/adr/0009-example.md",
                           ADR_TEMPLATE.replace(f"## {section}\n", "## Removed\n"))
                self.assertTrue(any(section in f.reason for f in self.check()),
                                f"missing {section} must be reported")
        self.write("docs/adr/0009-example.md", ADR_TEMPLATE)
    def test_allows_only_the_two_omitted_sections_on_legacy_adr(self) -> None:
        legacy = ADR_TEMPLATE.replace("0009-example", "0001-bootstrap-a-stdlib-only-cpp20-core")
        optional = ("\n## Options considered\n\nOptions text.\n", "\n## References\n\n- [the ADR process](../10_adr_process.md)\n")
        for passage in optional:
            self.write("docs/adr/0001-bootstrap-a-stdlib-only-cpp20-core.md", legacy.replace(passage, ""))
            self.assertEqual(self.check(), [])
        both = legacy.replace(optional[0], "").replace(optional[1], "")
        self.write("docs/adr/0001-bootstrap-a-stdlib-only-cpp20-core.md", both)
        self.assertEqual(self.check(), [])
        sections = {"Status": "Accepted on 2026-08-28.", "Context": "Context text.",
                    "Decision": "Decision text.", "Consequences": "Consequences text."}
        for section, body in sections.items():
            with self.subTest(section=section):
                missing = legacy.replace(f"## {section}\n\n{body}\n", "")
                self.write("docs/adr/0001-bootstrap-a-stdlib-only-cpp20-core.md", missing)
                self.assertTrue(any(section in f.reason for f in self.check()))
        self.write("docs/adr/0001-bootstrap-a-stdlib-only-cpp20-core.md", legacy)
    def test_ignores_fenced_registry_rows(self) -> None:
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE +
                   "\n```text\n| Fake | Invalid | Invalid | no authority | none |\n```\n")
        self.assertEqual(self.check(), [])
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE +
                   "\n| Hidden invalid | Maturity | Invalid | no authority | nobody |\n")
        self.assertTrue(any("vocabulary" in f.reason for f in self.check()))

    def test_requires_normative_authority_to_resolve_an_accepted_tracked_adr(self) -> None:
        rejected = ADR_TEMPLATE.replace("Accepted on 2026-08-28.", "Rejected on 2026-08-28.")
        self.write("docs/adr/0009-example.md", rejected)
        self.assertTrue(any("normative" in f.reason.lower() for f in self.check()))
        self.write("docs/adr/0009-example.md", ADR_TEMPLATE)
        missing = REGISTRY_TEMPLATE.replace("adr/0009-example.md", "adr/0008-missing.md")
        self.write("docs/08_contract_registry.md", missing)
        self.assertTrue(any("normative" in f.reason.lower() for f in self.check()))
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE)
        external = REGISTRY_TEMPLATE.replace("[ADR-0009](adr/0009-example.md)",
                                             "[ADR-0009](https://example.test/adr/0009.md)")
        self.write("docs/08_contract_registry.md", external)
        self.assertTrue(any("normative" in f.reason.lower() for f in self.check()))

    def test_rejects_authority_substring_spoofs(self) -> None:
        spoof = REGISTRY_TEMPLATE.replace("[Issue #35](https://github.com/tetsuh/sitometron/issues/35)",
                                          "[fake](not-an-adrift.txt)")
        self.write("docs/08_contract_registry.md", spoof)
        self.assertTrue(any("planned" in f.reason.lower() for f in self.check()))
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE)
        self.write("docs/99_planned.md", BANNER.replace("[Issue #35](https://github.com/tetsuh/sitometron/issues/35)",
                                                        "[fake](not-an-adrift.txt)"))
        self.assertTrue(any("banner" in f.reason.lower() for f in self.check()))

    def date_findings(self, status: str) -> list:
        self.write("docs/adr/0009-example.md",
                   ADR_TEMPLATE.replace("Accepted on 2026-08-28.", status))
        return [f for f in self.check() if "date" in f.reason]

    def test_requires_one_real_calendar_decision_date_for_every_status(self) -> None:
        rejected = {
            "no date on Accepted": "Accepted.",
            "no date on Proposed": "Proposed.",
            "no date on Rejected": "Rejected.",
            "impossible month and day": "Accepted — 2026-99-99.",
            "impossible February day": "Accepted — 2026-02-30.",
            "non-leap February 29": "Accepted — 2026-02-29.",
            "impossible month": "Accepted — 2026-13-01.",
            "two different dates": "Accepted — 2026-01-01 and 2026-02-02.",
        }
        for label, status in rejected.items():
            with self.subTest(case=label):
                self.assertTrue(self.date_findings(status), f"{label} must be reported")
        for status in ["Accepted — 2026-02-28.", "Accepted on 2024-02-29.",
                       "Proposed on 2026-08-30.", "Rejected 2026-08-30."]:
            with self.subTest(accepted=status):
                self.assertEqual(self.date_findings(status), [], status)
        self.write("docs/adr/0009-example.md", ADR_TEMPLATE)

    def test_reports_an_adr_directory_without_decision_records(self) -> None:
        (self.root / "docs/adr/0009-example.md").unlink()
        self.assertTrue(self.check())

    def test_rejects_unknown_registry_vocabulary(self) -> None:
        for original, replacement in [
            ("| Implemented surface | Normative |", "| Implemented surface | Final |"),
            ("Normative | Implemented |", "Normative | Done |"),
        ]:
            with self.subTest(replacement=replacement):
                self.write("docs/08_contract_registry.md",
                           REGISTRY_TEMPLATE.replace(original, replacement))
                self.assertTrue(any("vocabulary" in f.reason.lower() or "value" in f.reason.lower()
                                    for f in self.check()))

    def test_requires_accepted_adr_authority_on_normative_rows(self) -> None:
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE.replace(
            "| Implemented surface | Normative | Implemented | Accepted [ADR-0009](adr/0009-example.md) |",
            "| Implemented surface | Normative | Implemented | Owner decision |"))
        self.assertTrue(any("normative" in f.reason.lower() for f in self.check()))

    def test_requires_traceable_authority_only_for_planned_maturity(self) -> None:
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE.replace(
            "| Future surface | Planned | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment |",
            "| Future surface | Planned | Planned | To be decided |"))
        findings = self.check()
        self.assertTrue(any("planned" in f.reason.lower() for f in findings))
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE)
        self.assertEqual(self.check(), [],
                         "an implementation status of Planned on a Normative row is not a finding")

    def test_requires_an_authority_link_in_every_planned_banner(self) -> None:
        self.write("docs/99_planned.md", "# Planned\n\n" + BANNER)
        self.assertEqual(self.check(), [])
        self.write("docs/99_planned.md", "# Planned\n\n" + BANNER.replace(
            "[Issue #35](https://github.com/tetsuh/sitometron/issues/35)", "someone"))
        self.assertTrue(any("banner" in f.reason.lower() for f in self.check()))
        self.write("docs/99_bypass.md", "> **Planned, not yet normative:** Issue/ADR #NN has no owner.\n")
        self.assertTrue(any("banner" in f.reason.lower() for f in self.check()))

    def test_requires_every_expected_issue_form_field(self) -> None:
        forms = {
            ".github/ISSUE_TEMPLATE/feature.yml": self.module.FEATURE_FIELDS,
            ".github/ISSUE_TEMPLATE/adr.yml": self.module.ADR_FIELDS,
            ".github/ISSUE_TEMPLATE/gate.yml": self.module.GATE_FIELDS,
        }
        for path, fields in forms.items():
            for omitted in fields:
                with self.subTest(path=path, omitted=omitted):
                    kept = [(i, self.module.FORM_CONTRACTS[path][i][0]) for i in fields if i != omitted]
                    self.write(path, issue_form(kept, checkbox_ids=tuple(
                        i for i in fields if i != omitted and self.module.FORM_CONTRACTS[path][i][1] == "checkboxes")))
                    self.assertTrue(
                        any(omitted in f.reason for f in self.check()),
                        f"omitting {omitted} from {path} must be reported")
            self.write(path, self.form(path))

    def test_ignores_form_blocks_that_declare_no_identifier(self) -> None:
        text = ("name: X\ndescription: Y\nbody:\n"
                "  - type: markdown\n    attributes:\n      value: Intro text.\n"
                "  - type: textarea\n    id: summary\n    attributes:\n      label: Summary\n"
                "    validations:\n      required: true\n")
        blocks = self.module.parse_issue_form(text)
        self.assertEqual(sorted(blocks), ["summary"])
        self.assertTrue(blocks["summary"].required)

    def test_treats_a_checkbox_block_as_required_when_any_option_is(self) -> None:
        text = ("name: X\nbody:\n  - type: checkboxes\n    id: provenance\n    attributes:\n"
                "      label: L\n      options:\n        - label: A\n          required: true\n"
                "        - label: B\n          required: false\n")
        self.assertTrue(self.module.parse_issue_form(text)["provenance"].required)
        relaxed = text.replace("required: true", "required: false")
        self.assertFalse(self.module.parse_issue_form(relaxed)["provenance"].required)

    def test_reports_a_registry_row_with_the_wrong_column_count(self) -> None:
        self.write("docs/08_contract_registry.md", REGISTRY_TEMPLATE.replace(
            "| Future surface | Planned | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment | Phase 1 |",
            "| Future surface | Planned | [Issue #35](https://github.com/tetsuh/sitometron/issues/35) tracks assignment | Phase 1 |"))
        findings = self.check()
        self.assertTrue(any("columns" in f.reason for f in findings), findings)

    def test_ignores_a_banner_shown_inside_fenced_code(self) -> None:
        self.write("docs/99_example.md",
                   "# Example\n\n```markdown\n> " + BANNER.replace("> ", "").splitlines()[0] + "\n```\n")
        self.assertEqual(self.check(), [])

    def test_reports_issue_form_contract_drift(self) -> None:
        path = ".github/ISSUE_TEMPLATE/feature.yml"
        conforming = self.form(path)
        self.write(path, conforming.replace("label: Summary\n", "label: Summary drift\n", 1))
        self.assertTrue(any("expected 'Summary'" in f.reason for f in self.check()))

        self.write(path, conforming.replace("- type: input\n    id: phase", "- type: textarea\n    id: phase", 1))
        self.assertTrue(any("field 'phase' has kind" in f.reason for f in self.check()))

        duplicate = conforming.replace("label: Acceptance criteria", "label: Summary", 1)
        self.write(path, duplicate)
        self.assertTrue(any("duplicate expected field label 'Summary'" in f.reason for f in self.check()))

    def test_requires_expected_fields_to_be_required(self) -> None:
        self.write(".github/ISSUE_TEMPLATE/feature.yml", self.form(".github/ISSUE_TEMPLATE/feature.yml", required=False))
        self.assertTrue(any("required" in f.reason.lower() for f in self.check()))

    def test_requires_every_pull_request_template_field(self) -> None:
        for omitted in self.module.PULL_REQUEST_FIELDS:
            with self.subTest(omitted=omitted):
                kept = [f for f in self.module.PULL_REQUEST_FIELDS if f != omitted]
                self.write(".github/pull_request_template.md",
                           "".join(f"- {field}:\n" for field in kept))
                self.assertTrue(any(omitted in f.reason for f in self.check()))

    def test_rejects_prose_spoofed_duplicated_or_fenced_field_lines(self) -> None:
        fields = list(self.module.PULL_REQUEST_FIELDS)
        spoofed = fields[0]
        body = "".join(f"- {field}:\n" for field in fields[1:])
        cases = {
            "prose mention": f"The {spoofed}: is recorded in the summary above.\n" + body,
            "heading mention": f"## {spoofed}\n" + body,
            "inside fenced code": f"```text\n- {spoofed}:\n```\n" + body,
        }
        for label, text in cases.items():
            with self.subTest(case=label):
                self.write(".github/pull_request_template.md", text)
                self.assertTrue(any(spoofed in f.reason for f in self.check()), label)
        self.write(".github/pull_request_template.md",
                   f"- {spoofed}:\n" + body + f"- {spoofed}:\n")
        self.assertTrue(any("declared 2 times" in f.reason for f in self.check()))

    def test_reports_missing_governance_sources(self) -> None:
        for path in [".github/pull_request_template.md", ".github/ISSUE_TEMPLATE/gate.yml",
                     "docs/08_contract_registry.md"]:
            with self.subTest(path=path):
                (self.root / path).unlink()
                self.assertTrue(self.check())
                self.setUp()

    def test_main_returns_zero_when_clean_and_one_otherwise(self) -> None:
        with mock.patch.object(self.module, "tracked_files", lambda root: self.tracked()):
            self.assertEqual(self.module.main([], io.StringIO(), self.root), 0)
            self.write("docs/adr/0009-example.md", ADR_TEMPLATE.replace("Accepted on", "Draft on"))
            self.assertEqual(self.module.main([], io.StringIO(), self.root), 1)

    def test_main_fails_closed_when_git_enumeration_fails(self) -> None:
        def failing(root: object) -> list[str]:
            raise self.module.ValidationError("git ls-files failed")

        with mock.patch.object(self.module, "tracked_files", failing):
            self.assertEqual(self.module.main([], io.StringIO(), self.root), 1)

    def test_main_rejects_unknown_arguments(self) -> None:
        for arguments in [["--repository", str(self.root)], ["--unknown"], ["extra"], ["-h"]]:
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    self.module.main(arguments, io.StringIO(), self.root)


@unittest.skipUnless(VALIDATOR.is_file(), "governance validator is not implemented")
class RepositoryBaselineTest(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_validator()

    def test_repository_satisfies_every_governance_invariant(self) -> None:
        tracked = self.module.tracked_files(REPOSITORY_ROOT)
        findings = self.module.check_repository(REPOSITORY_ROOT, tracked)
        self.assertEqual(findings, [], f"repository governance findings: {findings}")

    def test_repository_templates_declare_the_frozen_fields(self) -> None:
        root = REPOSITORY_ROOT
        gate = self.module.parse_issue_form(
            (root / ".github/ISSUE_TEMPLATE/gate.yml").read_text(encoding="utf-8"))
        for field in self.module.GATE_FIELDS:
            self.assertIn(field, gate)
            self.assertTrue(gate[field].required, field)
        pull_request = (root / ".github/pull_request_template.md").read_text(encoding="utf-8")
        for field in self.module.PULL_REQUEST_FIELDS:
            self.assertIn(field, pull_request)


if __name__ == "__main__":
    unittest.main()
