#!/usr/bin/env python3
"""Validate repository-owned governance invariants without network access.

The validator checks only mechanical, repository-format invariants: ADR status
vocabulary and required sections, Contract Registry maturity and implementation
vocabularies with their authority rules, Planned-not-normative banner authority,
and the fields that the Issue forms and the pull-request template must declare.
It never calls a GitHub API and never infers human authorization, provenance, or
Gate evidence; those remain human and provider review evidence.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO

ADR_DIRECTORY = "docs/adr"
ADR_TEMPLATE = f"{ADR_DIRECTORY}/template.md"
ADR_INDEX = f"{ADR_DIRECTORY}/README.md"
REGISTRY_PATH = "docs/08_contract_registry.md"
FEATURE_FORM = ".github/ISSUE_TEMPLATE/feature.yml"
ADR_FORM = ".github/ISSUE_TEMPLATE/adr.yml"
GATE_FORM = ".github/ISSUE_TEMPLATE/gate.yml"
PULL_REQUEST_TEMPLATE = ".github/pull_request_template.md"

ADR_STATUSES = ("Proposed", "Accepted", "Rejected", "Deprecated", "Superseded")
ADR_SECTIONS = ("Status", "Context", "Decision", "Consequences")
DATE_PATTERN = re.compile(r"\b\d{4}-\d{2}-\d{2}\b")
MATURITY_VALUES = ("Planned", "Normative", "Deprecated", "Superseded")
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
AUTHORITY_PATTERN = re.compile(r"\[[^\]]+\]\([^)]*(?:issues|adr)[^)]*\)", re.IGNORECASE)
ACCEPTED_ADR_PATTERN = re.compile(r"Accepted\s+\[ADR-\d{4}\]\([^)]+\)")
HEADING_PATTERN = re.compile(r"\A {0,3}#{1,6}[ \t]+(.*?)[ \t]*#*[ \t]*\Z")
STATUS_PATTERN = re.compile(r"\A(" + "|".join(ADR_STATUSES) + r")\b")
FORM_ITEM_PATTERN = re.compile(r"\A  - type: ([a-z]+)\s*\Z")
FORM_ID_PATTERN = re.compile(r"\A    id: ([A-Za-z0-9_]+)\s*\Z")
FORM_LABEL_PATTERN = re.compile(r"\A      label: (.+?)\s*\Z")
FORM_REQUIRED_PATTERN = re.compile(r"\A\s+required: (true|false)\s*\Z")


class ValidationError(RuntimeError):
    """A fail-closed governance validation error."""


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


def tracked_files(root: Path | str) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=False
    )
    if result.returncode != 0:
        raise ValidationError(f"git ls-files failed with exit {result.returncode}")
    try:
        return [item for item in result.stdout.decode("utf-8").split("\0") if item]
    except UnicodeDecodeError as error:
        raise ValidationError("tracked paths are not valid UTF-8") from error


def _read(root: Path, relative_path: str) -> str:
    try:
        data = (root / relative_path).read_bytes()
    except OSError as error:
        raise ValidationError(f"cannot read {relative_path}: {error.strerror}") from error
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValidationError(f"{relative_path} is not valid UTF-8") from error


def parse_issue_form(text: str) -> dict[str, FormBlock]:
    """Parse one repository-format Issue form into its expected id/label/required blocks."""
    if "\r" in text:
        raise ValidationError("issue form must use LF line endings")
    blocks: dict[str, FormBlock] = {}
    kind: str | None = None
    identifier: str | None = None
    label: str | None = None
    required = False
    in_validation = False

    def close() -> None:
        nonlocal kind, identifier, label, required, in_validation
        if kind is None:
            return
        if identifier is None:
            raise ValidationError("issue form block has no id")
        if label is None:
            raise ValidationError(f"issue form block {identifier} has no label")
        if identifier in blocks:
            raise ValidationError(f"issue form declares duplicate id {identifier}")
        blocks[identifier] = FormBlock(identifier, kind, label, required)
        kind = identifier = label = None
        required = False
        in_validation = False

    for line in text.split("\n"):
        item = FORM_ITEM_PATTERN.match(line)
        if item is not None:
            close()
            kind = item.group(1)
            continue
        if kind is None:
            continue
        if (match := FORM_ID_PATTERN.match(line)) is not None:
            identifier = match.group(1)
            continue
        if (match := FORM_LABEL_PATTERN.match(line)) is not None:
            label = match.group(1)
            continue
        stripped = line.strip()
        if stripped in {"validations:", "options:"}:
            in_validation = True
            continue
        if stripped.startswith("- label:"):
            in_validation = True
            continue
        if (match := FORM_REQUIRED_PATTERN.match(line)) is not None:
            expected_indent = 6 if kind != "checkboxes" else 10
            if in_validation and len(line) - len(line.lstrip(" ")) == expected_indent:
                required = match.group(1) == "true"
            continue
        if stripped.startswith("attributes:"):
            in_validation = False
    close()
    if not blocks:
        raise ValidationError("issue form declares no block")
    return blocks


def _adr_status(text: str, relative_path: str) -> list[Finding]:
    findings: list[Finding] = []
    sections = _sections(text)
    for section in ADR_SECTIONS:
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


def _sections(text: str) -> dict[str, str]:
    sections: dict[str, str] = {}
    current: str | None = None
    fenced = False
    for line in text.split("\n"):
        if re.match(r"\A {0,3}(```|~~~)", line):
            fenced = not fenced
            continue
        if fenced:
            continue
        heading = HEADING_PATTERN.match(line)
        if heading is not None:
            current = heading.group(1).strip()
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


def _registry_rows(text: str) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in text.split("\n"):
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 5 or set("".join(cells)) <= set("-: "):
            continue
        if cells[1] == "Maturity":
            continue
        rows.append(cells)
    return rows


def check_registry(root: Path, tracked: Sequence[str]) -> list[Finding]:
    if REGISTRY_PATH not in tracked:
        return [Finding(REGISTRY_PATH, "the Contract Registry is not tracked")]
    findings: list[Finding] = []
    rows = _registry_rows(_read(root, REGISTRY_PATH))
    if not rows:
        return [Finding(REGISTRY_PATH, "the Contract Registry declares no surface row")]
    for surface, maturity, implementation, authority, *_ in rows:
        where = f"{REGISTRY_PATH} [{surface}]"
        if maturity not in MATURITY_VALUES:
            findings.append(Finding(where, f"maturity value {maturity!r} is outside the vocabulary"))
        if implementation not in IMPLEMENTATION_VALUES:
            findings.append(Finding(
                where, f"implementation value {implementation!r} is outside the vocabulary"))
        if maturity == "Normative" and ACCEPTED_ADR_PATTERN.search(authority) is None:
            findings.append(Finding(where, "Normative row does not name an Accepted ADR authority"))
        if maturity == "Planned" and AUTHORITY_PATTERN.search(authority) is None:
            findings.append(Finding(
                where, "Planned-maturity row does not name a traceable Issue or design authority"))
    return findings


def check_banners(root: Path, tracked: Sequence[str]) -> list[Finding]:
    findings: list[Finding] = []
    for path in sorted(path for path in tracked if path.endswith(".md")):
        for number, line in enumerate(_read(root, path).split("\n"), 1):
            if BANNER_MARKER not in line or BANNER_SPECIMEN in line:
                continue
            if AUTHORITY_PATTERN.search(line) is None:
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


def parse_arguments(arguments: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[1])
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None, stdout: TextIO = sys.stdout) -> int:
    options = parse_arguments(arguments)
    try:
        findings = check_repository(options.repository, tracked_files(options.repository))
    except ValidationError as error:
        print(f"governance: ERROR {error}", file=sys.stderr)
        return 1
    for finding in findings:
        print(f"governance: {finding}", file=stdout)
    print(f"governance: {len(findings)} finding(s)", file=stdout)
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
