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
from datetime import date
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
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
    resolve_local_path,
    split_local_target,
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
REGISTRY_HEADER = ("Contract surface", "Maturity", "Implementation", "Normative or design authority", "Owner")
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
    "RED command", "GREEN command and result",
    "REFACTOR command and result, or N/A with reason",
    "Exact-head owner authorization", "Selected merge method", "Auto-merge not enabled",
)

BANNER_MARKER = "**Planned, not yet normative:**"
BANNER_SPECIMEN = "Issue/ADR #NN"
LINK_PATTERN = re.compile(r"\[([^\]]+)\]\(([^)]*)\)")
ISSUE_URL_PATTERN = re.compile(r"\Ahttps://github\.com/tetsuh/sitometron/issues/[1-9][0-9]*\Z")
ADR_LABEL_PATTERN = re.compile(r"\AADR-[0-9]{4}\Z", re.IGNORECASE)
ADR_PATH_PATTERN = re.compile(r"\Adocs/adr/[0-9]{4}-[^/]+\.md\Z")
STATUS_PATTERN = re.compile(r"\A(?:" + "|".join(ADR_STATUSES) + r")\b")
FORM_ITEM_PATTERN = re.compile(r"\A {2}- type: (\w+)\Z")
FORM_ID_PATTERN = re.compile(r"\A {4}id: (\w+)\Z")
FORM_LABEL_PATTERN = re.compile(r"\A {6}label: (\S.*)\Z")
FORM_REQUIRED_PATTERN = re.compile(r"\A( +)required: (true|false)\Z")
FORM_CONTRACTS = {
    FEATURE_FORM: dict(zip(FEATURE_FIELDS, zip("Summary|Phase and Gate Issue|Reference documents and applicable sections|Requirement IDs or N/A|Target files or components|Dependencies and blockers|Contract Registry rows and transitions or N/A|ADR required?|ADR link and decision, or explicit not-required reason|Acceptance criteria|Clean-room confirmation".split("|"), "textarea|input|textarea|textarea|textarea|textarea|textarea|dropdown|textarea|textarea|checkboxes".split("|")))),
    ADR_FORM: dict(zip(ADR_FIELDS, zip("Context and decision to make|Phase and Gate Issue|Reference documents and applicable sections|Target component or contract surface|Dependencies and blockers|Requirement IDs or N/A|Contract Registry rows and intended transitions|Options requiring owner decision|Acceptance criteria|Clean-room confirmation".split("|"), "textarea|input|textarea|textarea|textarea|textarea|textarea|textarea|textarea|checkboxes".split("|")))),
    GATE_FORM: dict(zip(GATE_FIELDS, zip("Phase and Milestone|Requirement IDs or N/A|Entry gate|Horizontal design review|Planned-section authority banners|Contract Registry disposition or N/A|ADR decisions or N/A|Exit gate|Evidence|Clean-room confirmation".split("|"), "input|textarea|textarea|textarea|checkboxes|textarea|textarea|textarea|textarea|checkboxes".split("|")))),
}
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


def _authority_links(text: str) -> list[tuple[str, str]]:
    links: list[tuple[str, str]] = []
    for label, raw_destination in LINK_PATTERN.findall(text):
        destination = raw_destination.strip()
        if destination.startswith("<"):
            end = destination.find(">")
            if end <= 1:
                continue
            destination = destination[1:end]
        else:
            destination = destination.split(None, 1)[0]
        if destination:
            links.append((label.strip(), destination))
    return links


def _is_issue_authority(destination: str) -> bool:
    return ISSUE_URL_PATTERN.fullmatch(destination) is not None


def _resolve_adr(source: str, destination: str, tracked: Sequence[str]) -> str | None:
    try:
        path, fragment = split_local_target(destination)
        if fragment or not path:
            return None
        resolved = resolve_local_path(source, destination)
    except ValidationError:
        return None
    return resolved if ADR_PATH_PATTERN.fullmatch(resolved) and resolved in tracked else None


def _names_authority(source: str, text: str, tracked: Sequence[str]) -> bool:
    return any(_is_issue_authority(destination) or _resolve_adr(source, destination, tracked) is not None
               for _, destination in _authority_links(text))


def _accepted_adr_authority(source: str, text: str, root: Path, tracked: Sequence[str]) -> bool:
    for label, destination in _authority_links(text):
        if not ADR_LABEL_PATTERN.fullmatch(label):
            continue
        resolved = _resolve_adr(source, destination, tracked)
        if resolved is None:
            continue
        sections = _sections(_read(root, resolved))
        status = sections.get("Status", "").strip()
        if (match := STATUS_PATTERN.match(status)) is not None and match.group(0) == "Accepted":
            return True
    return False


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
        self.validation_context: str | None = None
        self.in_option = False

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
            indent = len(match.group(1))
            valid_context = (
                self.kind != "checkboxes" and self.validation_context == "validations" and
                indent == TEXT_REQUIRED_INDENT
            ) or (self.kind == "checkboxes" and self.validation_context == "options" and
                  self.in_option and indent == CHECKBOX_REQUIRED_INDENT)
            if valid_context:
                declared = match.group(2) == "true"
                self.required = self.required or declared if self.kind == "checkboxes" else declared
            return
        indent = len(line) - len(line.lstrip(" "))
        stripped = line.strip()
        if indent == 4 and stripped == "validations:":
            self.validation_context = "validations"
            self.in_option = False
        elif indent == 6 and stripped == "options:":
            self.validation_context = "options"
            self.in_option = False
        elif indent == 8 and stripped.startswith("- label:"):
            self.in_option = self.validation_context == "options"
        elif indent <= 4:
            self.validation_context = None
            self.in_option = False

    def build(self) -> FormBlock | None:
        if self.identifier is None:
            if self.kind == "markdown":
                # `- type: markdown` help text legitimately declares no id and no label.
                return None
            raise ValidationError(f"issue form {self.kind} block has no id")
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
        return findings
    findings.extend(_adr_decision_date(body, relative_path))
    return findings


def _adr_decision_date(body: str, relative_path: str) -> list[Finding]:
    """Every ADR status carries exactly one real ISO-8601 calendar decision date."""
    candidates = DATE_PATTERN.findall(body)
    if not candidates:
        return [Finding(relative_path, "ADR status carries no ISO-8601 decision date")]
    if len(set(candidates)) > 1:
        return [Finding(relative_path, "ADR status carries more than one decision date")]
    try:
        date.fromisoformat(candidates[0])
    except ValueError:
        return [Finding(
            relative_path, f"ADR decision date {candidates[0]!r} is not a real calendar date")]
    return []


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
        if tuple(cells) == REGISTRY_HEADER or (
                len(cells) == REGISTRY_COLUMNS and all(set(cell) <= set("-: ") for cell in cells)):
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
        if maturity == "Normative" and not _accepted_adr_authority(
                REGISTRY_PATH, authority, root, tracked):
            findings.append(Finding(where, "Normative row does not name an Accepted ADR authority"))
        if maturity == "Planned" and not _names_authority(REGISTRY_PATH, authority, tracked):
            findings.append(Finding(
                where, "Planned-maturity row does not name a traceable Issue or design authority"))
    return findings


def check_banners(root: Path, tracked: Sequence[str]) -> list[Finding]:
    findings: list[Finding] = []
    for path in sorted(path for path in tracked if path.endswith(".md")):
        for number, line in enumerate(_content_lines(_read(root, path)), 1):
            if BANNER_MARKER not in line or (
                    path == "docs/development_workflow.md" and number == 236 and
                    line == "> **Planned, not yet normative:** Issue/ADR #NN owns this mechanism. Implementers must not treat"):
                continue
            if not _names_authority(path, line, tracked):
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
    contract = FORM_CONTRACTS[relative_path]
    for label in {label for label, _ in contract.values()}:
        if sum(block.label == label for block in blocks.values()) > 1:
            findings.append(Finding(relative_path, f"duplicate expected field label {label!r}"))
    for identifier in expected:
        block = blocks.get(identifier)
        if block is None:
            findings.append(Finding(relative_path, f"missing required field {identifier!r}"))
            continue
        label, kind = contract[identifier]
        if block.label != label:
            findings.append(Finding(relative_path, f"field {identifier!r} has label {block.label!r}, expected {label!r}"))
        if block.kind != kind:
            findings.append(Finding(relative_path, f"field {identifier!r} has kind {block.kind!r}, expected {kind!r}"))
        if not block.required:
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
    lines = [line.strip() for line in _content_lines(_read(root, PULL_REQUEST_TEMPLATE))]
    findings: list[Finding] = []
    for field in PULL_REQUEST_FIELDS:
        occurrences = lines.count(f"- {field}:")
        if occurrences == 0:
            findings.append(Finding(
                PULL_REQUEST_TEMPLATE,
                f"missing required field line {f'- {field}:'!r}; prose mentions do not count"))
        elif occurrences > 1:
            findings.append(Finding(
                PULL_REQUEST_TEMPLATE, f"required field {field!r} is declared {occurrences} times"))
    return findings


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
