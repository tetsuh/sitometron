#!/usr/bin/env python3
"""Validate repository-owned governance invariants without network access.

Git enumeration, the argument contract, the fail-closed error type, and the
finding report format are shared with `check_relative_links.py` so that both
Phase 0A validators behave identically at their boundaries.

The validator checks only mechanical, repository-format invariants: ADR status
vocabulary and required sections, Contract Registry maturity and implementation
vocabularies with their authority rules, Planned-not-normative banner authority,
and the fields that the Issue forms and the pull-request template must declare.
It never calls a GitHub API and never infers human authorization, provenance, or
Gate evidence; those remain human and provider review evidence.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO

TOOLS_DIRECTORY = Path(__file__).resolve().parent
if str(TOOLS_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIRECTORY))

from check_relative_links import (  # noqa: E402
    REPOSITORY_ROOT,
    ValidationError,
    parse_arguments,
    run_validator,
    tracked_files,
    validated_repository,
    _content_lines,
)

__all__ = ["ValidationError", "tracked_files", "validated_repository"]

ADR_DIRECTORY = "docs/adr"
ADR_TEMPLATE = f"{ADR_DIRECTORY}/template.md"
ADR_INDEX = f"{ADR_DIRECTORY}/README.md"
REGISTRY_PATH = "docs/08_contract_registry.md"
FEATURE_FORM = ".github/ISSUE_TEMPLATE/feature.yml"
ADR_FORM = ".github/ISSUE_TEMPLATE/adr.yml"
GATE_FORM = ".github/ISSUE_TEMPLATE/gate.yml"
PULL_REQUEST_TEMPLATE = ".github/pull_request_template.md"

ADR_STATUSES = ("Proposed", "Accepted", "Rejected", "Deprecated", "Superseded")
ADR_SECTIONS = ("Status", "Context", "Decision", "Consequences", "Options considered", "References")
ADR_LEGACY_PATH = f"{ADR_DIRECTORY}/0001-bootstrap-a-stdlib-only-cpp20-core.md"
ADR_LEGACY_SECTIONS = ADR_SECTIONS[:4]
DATE_PATTERN = re.compile(r"\b\d{4}-\d{2}-\d{2}\b")
MATURITY_VALUES = ("Planned", "Normative", "Deprecated", "Superseded")
REGISTRY_COLUMNS = 5
IMPLEMENTATION_VALUES = ("Planned", "In progress", "Implemented", "Removed")

FEATURE_FIELDS = (
    "summary", "phase", "references", "requirements", "targets", "dependencies", "contracts",
    "adr", "adr_authority", "acceptance", "provenance",
)
ADR_FIELDS = (
    "context", "phase", "references", "target", "dependencies", "requirements", "contracts",
    "options", "acceptance", "provenance",
)
GATE_FIELDS = (
    "phase", "requirements", "entry", "horizontal_review", "planned_banners", "contracts", "adr",
    "exit", "evidence", "provenance",
)
PULL_REQUEST_FIELDS = (
    "Phase", "Gate Issue", "Requirement IDs or N/A", "ADR or N/A", "Affected rows or N/A",
    "RED command", "GREEN command and result", "REFACTOR command and result",
    "Exact-head owner authorization", "Selected merge method", "Auto-merge not enabled",
)

BANNER_MARKER = "**Planned, not yet normative:**"
BANNER_SPECIMEN = "Issue/ADR #NN"
LINK_PATTERN = re.compile(r"\[[^\]]+\]\(([^)]*)\)")
AUTHORITY_TOKENS = ("issues", "adr")
ACCEPTED_ADR_PATTERN = re.compile(r"Accepted \[ADR-\d{4}\]\([^)]+\)")
STATUS_PATTERN = re.compile(r"\A(?:" + "|".join(ADR_STATUSES) + r")\b")
FORM_ITEM_PATTERN = re.compile(r"\A {2}- type: (\w+)\Z")
FORM_ID_PATTERN = re.compile(r"\A {4}id: (\w+)\Z")
FORM_LABEL_PATTERN = re.compile(r"\A {6}label: (\S.*)\Z")
FORM_REQUIRED_PATTERN = re.compile(r"\A( +)required: (true|false)\Z")
TEXT_REQUIRED_INDENT = 6
CHECKBOX_REQUIRED_INDENT = 10


@dataclass(frozen=True)
class Finding:
    source: str
    reason: str

    def __str__(self) -> str:
        return f"{self.source}: {self.reason}"


@dataclass(frozen=True)
class FormBlock:
    identifier: str
    kind: str
    label: str
    required: bool


def _names_authority(text: str) -> bool:
    return any(
        token in target.lower() for target in LINK_PATTERN.findall(text)
        for token in AUTHORITY_TOKENS
    )


def _read(root: Path, relative_path: str) -> str:
    try:
        data = (root / relative_path).read_bytes()
    except OSError as error:
        raise ValidationError(f"cannot read {relative_path}: {error.strerror}") from error
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValidationError(f"{relative_path} is not valid UTF-8") from error


class _FormBlockBuilder:
    """Accumulate one Issue-form block while its lines are read."""

    def __init__(self, kind: str) -> None:
        self.kind = kind
        self.identifier: str | None = None
        self.label: str | None = None
        self.required = False
        self.in_validation = False

    @property
    def required_indent(self) -> int:
        return CHECKBOX_REQUIRED_INDENT if self.kind == "checkboxes" else TEXT_REQUIRED_INDENT

    def read(self, line: str) -> None:
        if (match := FORM_ID_PATTERN.match(line)) is not None:
            self.identifier = match.group(1)
            return
        if (match := FORM_LABEL_PATTERN.match(line)) is not None:
            self.label = match.group(1).rstrip()
            return
        if (match := FORM_REQUIRED_PATTERN.match(line)) is not None:
            if self.in_validation and len(match.group(1)) == self.required_indent:
                declared = match.group(2) == "true"
                # A checkbox block enforces an answer when any single option is required.
                self.required = self.required or declared if self.kind == "checkboxes" else declared
            return
        stripped = line.strip()
        if stripped in {"validations:", "options:"} or stripped.startswith("- label:"):
            self.in_validation = True
        elif stripped.startswith("attributes:"):
            self.in_validation = False

    def build(self) -> FormBlock | None:
        if self.identifier is None:
            # `- type: markdown` help text legitimately declares no id and no label.
            return None
        if self.label is None:
            raise ValidationError(f"issue form block {self.identifier} has no label")
        return FormBlock(self.identifier, self.kind, self.label, self.required)


def parse_issue_form(text: str) -> dict[str, FormBlock]:
    """Parse one repository-format Issue form into its expected id/label/required blocks."""
    if "\r" in text:
        raise ValidationError("issue form must use LF line endings")
    blocks: dict[str, FormBlock] = {}
    builder: _FormBlockBuilder | None = None

    def close(current: _FormBlockBuilder | None) -> None:
        if current is None:
            return
        block = current.build()
        if block is None:
            return
        if block.identifier in blocks:
            raise ValidationError(f"issue form declares duplicate id {block.identifier}")
        blocks[block.identifier] = block

    for line in text.split("\n"):
        item = FORM_ITEM_PATTERN.match(line)
        if item is not None:
            close(builder)
            builder = _FormBlockBuilder(item.group(1))
        elif builder is not None:
            builder.read(line)
    close(builder)
    if not blocks:
        raise ValidationError("issue form declares no block")
    return blocks


def _adr_status(text: str, relative_path: str) -> list[Finding]:
    findings: list[Finding] = []
    sections = _sections(text)
    required_sections = ADR_LEGACY_SECTIONS if relative_path == ADR_LEGACY_PATH else ADR_SECTIONS
    for section in required_sections:
        if section not in sections:
            findings.append(Finding(relative_path, f"missing required ADR section {section!r}"))
    body = sections.get("Status", "").strip()
    if not body:
        findings.append(Finding(relative_path, "ADR Status section is empty"))
        return findings
    if STATUS_PATTERN.match(body) is None:
        findings.append(Finding(
            relative_path, f"ADR status value is outside the vocabulary {ADR_STATUSES}"))
    elif DATE_PATTERN.search(body) is None and not body.startswith("Proposed"):
        findings.append(Finding(relative_path, "ADR status carries no ISO-8601 decision date"))
    return findings


def _heading_text(line: str) -> str | None:
    indent = len(line) - len(line.lstrip(" "))
    if indent > 3:
        return None
    rest = line[indent:]
    level = len(rest) - len(rest.lstrip("#"))
    if not 1 <= level <= 6:
        return None
    rest = rest[level:]
    if not rest or rest[0] not in " \t":
        return None
    return rest.strip(" \t").rstrip("#").strip(" \t") or None


def _sections(text: str) -> dict[str, str]:
    sections: dict[str, str] = {}
    current: str | None = None
    for line in _content_lines(text):
        heading = _heading_text(line)
        if heading is not None:
            current = heading
            sections.setdefault(current, "")
            continue
        if current is not None:
            sections[current] += line + "\n"
    return sections


def check_adrs(root: Path, tracked: Sequence[str]) -> list[Finding]:
    findings: list[Finding] = []
    decisions = sorted(
        path for path in tracked
        if path.startswith(f"{ADR_DIRECTORY}/") and path.endswith(".md")
        and path not in {ADR_TEMPLATE, ADR_INDEX}
    )
    if not decisions:
        findings.append(Finding(ADR_DIRECTORY, "no tracked ADR decision record was found"))
    for path in decisions:
        findings.extend(_adr_status(_read(root, path), path))
    return findings


def _registry_rows(text: str) -> tuple[list[list[str]], list[int]]:
    """Return the surface rows of the Registry table and the lines that are malformed."""
    rows: list[list[str]] = []
    malformed: list[int] = []
    for number, line in enumerate(_content_lines(text), 1):
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if set("".join(cells)) <= set("-: ") or (len(cells) > 1 and cells[1] == "Maturity"):
            continue
        if len(cells) != REGISTRY_COLUMNS:
            malformed.append(number)
            continue
        rows.append(cells)
    return rows, malformed


def check_registry(root: Path, tracked: Sequence[str]) -> list[Finding]:
    if REGISTRY_PATH not in tracked:
        return [Finding(REGISTRY_PATH, "the Contract Registry is not tracked")]
    findings: list[Finding] = []
    rows, malformed = _registry_rows(_read(root, REGISTRY_PATH))
    findings.extend(
        Finding(f"{REGISTRY_PATH}:{number}",
                f"Registry row does not declare exactly {REGISTRY_COLUMNS} columns")
        for number in malformed
    )
    if not rows:
        findings.append(Finding(REGISTRY_PATH, "the Contract Registry declares no surface row"))
        return findings
    for surface, maturity, implementation, authority, *_ in rows:
        where = f"{REGISTRY_PATH} [{surface}]"
        if maturity not in MATURITY_VALUES:
            findings.append(Finding(where, f"maturity value {maturity!r} is outside the vocabulary"))
        if implementation not in IMPLEMENTATION_VALUES:
            findings.append(Finding(
                where, f"implementation value {implementation!r} is outside the vocabulary"))
        if maturity == "Normative" and ACCEPTED_ADR_PATTERN.search(authority) is None:
            findings.append(Finding(where, "Normative row does not name an Accepted ADR authority"))
        if maturity == "Planned" and not _names_authority(authority):
            findings.append(Finding(
                where, "Planned-maturity row does not name a traceable Issue or design authority"))
    return findings


def check_banners(root: Path, tracked: Sequence[str]) -> list[Finding]:
    findings: list[Finding] = []
    for path in sorted(path for path in tracked if path.endswith(".md")):
        for number, line in enumerate(_content_lines(_read(root, path)), 1):
            if BANNER_MARKER not in line or BANNER_SPECIMEN in line:
                continue
            if not _names_authority(line):
                findings.append(Finding(
                    f"{path}:{number}", "Planned banner does not name an owning Issue or ADR"))
    return findings


def _check_form(root: Path, relative_path: str, expected: Sequence[str],
                tracked: Sequence[str]) -> list[Finding]:
    if relative_path not in tracked:
        return [Finding(relative_path, "the Issue form is not tracked")]
    try:
        blocks = parse_issue_form(_read(root, relative_path))
    except ValidationError as error:
        return [Finding(relative_path, str(error))]
    findings: list[Finding] = []
    for identifier in expected:
        block = blocks.get(identifier)
        if block is None:
            findings.append(Finding(relative_path, f"missing required field {identifier!r}"))
        elif not block.required:
            findings.append(Finding(relative_path, f"field {identifier!r} is not required: true"))
    return findings


def check_issue_forms(root: Path, tracked: Sequence[str]) -> list[Finding]:
    findings: list[Finding] = []
    for relative_path, expected in (
        (FEATURE_FORM, FEATURE_FIELDS), (ADR_FORM, ADR_FIELDS), (GATE_FORM, GATE_FIELDS),
    ):
        findings.extend(_check_form(root, relative_path, expected, tracked))
    return findings


def check_pull_request_template(root: Path, tracked: Sequence[str]) -> list[Finding]:
    if PULL_REQUEST_TEMPLATE not in tracked:
        return [Finding(PULL_REQUEST_TEMPLATE, "the pull-request template is not tracked")]
    text = _read(root, PULL_REQUEST_TEMPLATE)
    return [
        Finding(PULL_REQUEST_TEMPLATE, f"missing required field {field!r}")
        for field in PULL_REQUEST_FIELDS if field not in text
    ]


def check_repository(root: Path | str, tracked: Sequence[str]) -> list[Finding]:
    """Return every governance finding for one repository."""
    root = Path(root)
    findings: list[Finding] = []
    for check in (check_adrs, check_registry, check_banners, check_issue_forms,
                  check_pull_request_template):
        try:
            findings.extend(check(root, tracked))
        except ValidationError as error:
            findings.append(Finding(check.__name__, str(error)))
    return findings


def main(
    arguments: Sequence[str] | None = None, stdout: TextIO = sys.stdout,
    repository: Path = REPOSITORY_ROOT,
) -> int:
    parse_arguments(arguments, __doc__)
    return run_validator("governance", check_repository, repository, stdout, tracked_files)


if __name__ == "__main__":
    raise SystemExit(main())
