#!/usr/bin/env python3
"""Validate repository-relative Markdown links and anchors without network access.

The validator resolves every local link against Git-tracked paths using a
platform-independent POSIX grammar, requires exact repository spelling on every
platform, and compares fragments against anchors emitted by an ATX-only heading
slug algorithm. Only the `http`, `https`, and `mailto` schemes are ignored;
every other scheme, any query on a local target, and every malformed or
ambiguous percent-escape fail closed.
"""

from __future__ import annotations

import argparse
import html
import re
import subprocess
import sys
import unicodedata
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TextIO

EXTERNAL_SCHEMES = frozenset({"http", "https", "mailto"})
MARKDOWN_SUFFIX = ".md"
SCHEME_PATTERN = re.compile(r"\A(\w[\w+.-]*):")
REFERENCE_USE_PATTERN = re.compile(r"(?<!\!)\[([^\]]+)\]\[([^\]]*)\]")
REFERENCE_DEFINITION_PATTERN = re.compile(r"\A {0,3}\[([^\]]+)\]:[ \t]*(.*)\Z")
MAX_LABEL_DEPTH = 8
INVALID_REFERENCE_TARGET = "<invalid-reference>"
MAX_DESTINATION_DEPTH = 8
HTML_TAG_PATTERN = re.compile(r"<[^>]*>")
PERCENT_PATTERN = re.compile(r"%(..?|\Z)", re.DOTALL)
VALID_ESCAPE_PATTERN = re.compile(r"\A[0-9A-Fa-f]{2}\Z")
DRIVE_PATTERN = re.compile(r"\A[A-Za-z]:")
MAX_HEADING_LEVEL = 6
MAX_HEADING_INDENT = 3
FORBIDDEN_DECODED = frozenset({"/", "\\"})
KEPT_CATEGORIES = ("L", "N", "M")
KEPT_CHARACTERS = frozenset({"-", "_"})


class ValidationError(RuntimeError):
    """A fail-closed link or anchor validation error."""


@dataclass(frozen=True)
class Finding:
    file: str
    line: int
    target: str
    reason: str

    def __str__(self) -> str:
        return f"{self.file}:{self.line}: {self.reason}: {self.target!r}"


def is_external(target: str) -> bool:
    match = SCHEME_PATTERN.match(target)
    return match is not None and match.group(1).lower() in EXTERNAL_SCHEMES


def _decode_once(raw: str, what: str) -> str:
    escapes = PERCENT_PATTERN.findall(raw)
    for escape in escapes:
        if VALID_ESCAPE_PATTERN.match(escape) is None:
            raise ValidationError(f"{what} contains a malformed percent-escape")
    for escape in escapes:
        if chr(int(escape, 16)) in FORBIDDEN_DECODED:
            raise ValidationError(f"{what} encodes a path separator")
    try:
        decoded = re.sub(
            rb"%([0-9A-Fa-f]{2})", lambda match: bytes([int(match.group(1), 16)]),
            raw.encode("utf-8"),
        ).decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValidationError(f"{what} is not valid UTF-8 after decoding") from error
    if any(unicodedata.category(character) == "Cc" for character in decoded):
        raise ValidationError(f"{what} contains a control character")
    if PERCENT_PATTERN.search(decoded) is not None:
        raise ValidationError(f"{what} remains percent-encoded after one decoding")
    return decoded


def split_local_target(target: str) -> tuple[str, str]:
    """Split one local target into a decoded (path, fragment) pair."""
    if is_external(target):
        raise ValidationError("target uses an ignored external scheme")
    if SCHEME_PATTERN.match(target) is not None:
        raise ValidationError("target uses an unsupported scheme")
    path_part, _, fragment_part = target.partition("#")
    if "?" in path_part or "?" in fragment_part:
        raise ValidationError("local target must not carry a query")
    return _decode_once(path_part, "path"), _decode_once(fragment_part, "fragment")


def resolve_local_path(source: str, target: str) -> str:
    """Resolve one decoded local path against its containing directory."""
    path, _ = split_local_target(target)
    if not path:
        return source
    if "\\" in path:
        raise ValidationError("local path contains a backslash")
    if path.startswith("/") or DRIVE_PATTERN.match(path) is not None:
        raise ValidationError("local path is absolute, a drive path, or a UNC path")
    components = path[:-1].split("/") if path.endswith("/") else path.split("/")
    if any(component in {"", "."} for component in components):
        raise ValidationError("local path contains an empty or dot component")
    parts = list(PurePosixPath(source).parent.parts)
    for component in components:
        if component == "..":
            if not parts:
                raise ValidationError("local path escapes the repository root")
            parts.pop()
        else:
            parts.append(component)
    if not parts:
        raise ValidationError("local path resolves to the repository root")
    return "/".join(parts)


def _strip_closing_hashes(heading: str) -> str:
    text = heading.rstrip(" \t")
    stripped = text.rstrip("#")
    if stripped != text and (not stripped or stripped[-1] in " \t"):
        return stripped.rstrip(" \t")
    return text


def heading_text(line: str) -> str | None:
    """Return the raw text of one ATX heading line, or None when it is not a heading."""
    indent = len(line) - len(line.lstrip(" "))
    if indent > MAX_HEADING_INDENT:
        return None
    rest = line[indent:]
    level = len(rest) - len(rest.lstrip("#"))
    if not 1 <= level <= MAX_HEADING_LEVEL:
        return None
    rest = rest[level:]
    if rest and rest[0] not in " \t":
        return None
    return rest.strip(" \t")


def _label_end(line: str, start: int) -> int | None:
    if start >= len(line) or line[start] != "[":
        return None
    depth = 1
    index = start + 1
    while index < len(line):
        character = line[index]
        if character == "\\":
            index += 2
            continue
        if character == "[":
            depth += 1
            if depth > MAX_LABEL_DEPTH:
                return None
        elif character == "]":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return None


def _replace_inline_labels(text: str) -> str:
    visible: list[str] = []
    index = 0
    while index < len(text):
        if text[index] == "[" and not _is_escaped(text, index):
            end = _label_end(text, index)
            if end is not None and end < len(text) and text[end] == "(":
                destination = _destination(text, end + 1)
                if destination is not None:
                    visible.append(text[index + 1:end - 1])
                    index = destination[1]
                    continue
        visible.append(text[index])
        index += 1
    return "".join(visible)


def _is_escaped(text: str, index: int) -> bool:
    slashes = 0
    index -= 1
    while index >= 0 and text[index] == "\\":
        slashes += 1
        index -= 1
    return slashes % 2 == 1


def _visible_text(heading: str) -> str:
    text = _strip_closing_hashes(heading)
    text = _replace_inline_labels(text)
    text = HTML_TAG_PATTERN.sub("", text)
    text = html.unescape(text)
    return re.sub(r"[`*~]|(?<![0-9A-Za-z])_|_(?![0-9A-Za-z])", "", text)


def slugify(heading: str) -> str:
    """Return the anchor slug emitted for one ATX heading's raw text."""
    text = unicodedata.normalize("NFC", _visible_text(heading)).casefold()
    text = re.sub(r"\s+", "-", text.strip())
    slug = "".join(
        character for character in text
        if unicodedata.category(character)[0] in KEPT_CATEGORIES or character in KEPT_CHARACTERS
    )
    if not slug:
        raise ValidationError(f"heading produces an empty anchor slug: {heading!r}")
    return slug


def _fence_delimiter(line: str) -> tuple[str, int, str] | None:
    """Return the (character, length, trailing text) of one valid fence line."""
    indent = len(line) - len(line.lstrip(" "))
    if indent > MAX_HEADING_INDENT or indent == len(line):
        return None
    marker = line[indent]
    if marker not in "`~":
        return None
    end = indent
    while end < len(line) and line[end] == marker:
        end += 1
    length = end - indent
    if length < 3:
        return None
    trailing = line[end:].strip(" \t")
    if marker == "`" and "`" in trailing:
        return None
    return marker, length, trailing


def _closes_fence(line: str, fence: tuple[str, int]) -> bool:
    marker = _fence_delimiter(line)
    return marker is not None and marker[0] == fence[0] and marker[1] >= fence[1] and not marker[2]


def _content_lines(markdown: str) -> list[str]:
    """Return lines outside correctly matched Markdown fenced code blocks."""
    lines: list[str] = []
    fence: tuple[str, int] | None = None
    for line in markdown.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if fence is not None:
            if _closes_fence(line, fence):
                fence = None
            lines.append("")
            continue
        marker = _fence_delimiter(line)
        if marker is None:
            lines.append(line)
            continue
        fence = (marker[0], marker[1])
        lines.append("")
    return lines


def _backtick_runs(line: str) -> list[tuple[int, int]]:
    """Return the half-open bounds of every maximal backtick run."""
    runs: list[tuple[int, int]] = []
    index = 0
    while index < len(line):
        if line[index] != "`":
            index += 1
            continue
        end = index + 1
        while end < len(line) and line[end] == "`":
            end += 1
        runs.append((index, end))
        index = end
    return runs


def strip_code_spans(line: str) -> str:
    """Blank inline code spans using equal-width maximal backtick runs."""
    runs = _backtick_runs(line)
    next_matching: list[int | None] = [None] * len(runs)
    latest: dict[int, int] = {}
    for index in range(len(runs) - 1, -1, -1):
        width = runs[index][1] - runs[index][0]
        next_matching[index] = latest.get(width)
        latest[width] = index

    characters = list(line)
    index = 0
    while index < len(runs):
        closing = next_matching[index]
        if closing is None:
            index += 1
            continue
        start = runs[index][0]
        end = runs[closing][1]
        characters[start:end] = " " * (end - start)
        index = closing + 1
    return "".join(characters)


def emitted_anchors(markdown: str) -> list[str]:
    """Return every anchor emitted by the ATX headings of one document, in order."""
    anchors: list[str] = []
    counts: dict[str, int] = {}
    for line in _content_lines(markdown):
        raw = heading_text(line)
        if raw is None:
            continue
        slug = slugify(raw)
        suffix = counts.get(slug, 0)
        candidate = slug if suffix == 0 else f"{slug}-{suffix}"
        used = set(anchors)
        while candidate in used:
            suffix += 1
            candidate = f"{slug}-{suffix}"
        counts[slug] = suffix + 1
        anchors.append(candidate)
    return anchors


def validated_repository(candidate: Path | str) -> Path:
    """Return one existing Git working tree, rejecting every other argument value."""
    resolved = Path(candidate).resolve()
    if not resolved.is_dir() or not (resolved / ".git").exists():
        raise ValidationError("--repository must name an existing Git working tree")
    return resolved


def tracked_files(root: Path | str) -> list[str]:
    repository = validated_repository(root)
    result = subprocess.run(
        ["git", "-C", str(repository), "ls-files", "-z"], capture_output=True, check=False
    )
    if result.returncode != 0:
        raise ValidationError(f"git ls-files failed with exit {result.returncode}")
    try:
        return [item for item in result.stdout.decode("utf-8").split("\0") if item]
    except UnicodeDecodeError as error:
        raise ValidationError("tracked paths are not valid UTF-8") from error


def _destination(line: str, start: int) -> tuple[str, int] | None:
    """Read one inline destination that starts just after `(`, honouring nesting."""
    depth = 1
    index = start
    while index < len(line) and depth <= MAX_DESTINATION_DEPTH:
        character = line[index]
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return line[start:index], index + 1
        index += 1
    return None


def _inline_target(raw: str) -> str:
    """Return the link destination of one inline `(...)` body without its title."""
    text = raw.strip()
    if text.startswith("<"):
        end = text.find(">")
        return text[1:end] if end != -1 else ""
    return text.split(" ", 1)[0].split("\t", 1)[0]


def inline_links(line: str) -> list[str]:
    """Return every inline link destination on one content line."""
    targets: list[str] = []
    index = 0
    while index < len(line):
        if line[index] != "[" or _is_escaped(line, index):
            index += 1
            continue
        end = _label_end(line, index)
        if end is None:
            index += 1
            continue
        if end >= len(line) or line[end] != "(":
            index = end
            continue
        destination = _destination(line, end + 1)
        if destination is None:
            index = end + 1
            continue
        target = _inline_target(destination[0])
        if target:
            targets.append(target)
        index = destination[1]
    return targets


def _reference_target(raw: str) -> str:
    """Return one normalized reference-definition destination or an invalid sentinel."""
    text = raw.strip(" \t")
    if text.startswith("<"):
        end = text.find(">")
        if end <= 1:
            return INVALID_REFERENCE_TARGET
        return text[1:end]
    target = text.split(" ", 1)[0].split("\t", 1)[0]
    return target or INVALID_REFERENCE_TARGET


def _reference_definitions(lines: Sequence[str]) -> dict[str, tuple[str, int]]:
    """Return the first definition of every reference label, as CommonMark requires."""
    definitions: dict[str, tuple[str, int]] = {}
    for number, line in enumerate(lines, 1):
        match = REFERENCE_DEFINITION_PATTERN.match(line)
        if match is not None:
            label = " ".join(match.group(1).split()).casefold()
            definitions.setdefault(label, (_reference_target(match.group(2)), number))
    return definitions


def _line_links(
    number: int, raw_line: str, definitions: dict[str, tuple[str, int]],
) -> list[tuple[int, str]]:
    definition = REFERENCE_DEFINITION_PATTERN.match(raw_line)
    if definition is not None:
        return [(number, _reference_target(definition.group(2)))]
    line = strip_code_spans(raw_line)
    links = [(number, target) for target in inline_links(line)]
    for match in REFERENCE_USE_PATTERN.finditer(line):
        label = " ".join((match.group(2) or match.group(1)).split()).casefold()
        links.append((number, definitions.get(label, (INVALID_REFERENCE_TARGET, 0))[0]))
    return links


def extract_links(markdown: str) -> list[tuple[int, str]]:
    """Return every (line number, raw target) link outside fenced code."""
    lines = _content_lines(markdown)
    definitions = _reference_definitions(lines)
    links: list[tuple[int, str]] = []
    for number, raw_line in enumerate(lines, 1):
        links.extend(_line_links(number, raw_line, definitions))
    return links


def read_markdown(root: Path, relative_path: str) -> str:
    """Read one tracked Markdown file, failing closed on I/O and encoding errors."""
    try:
        return (root / relative_path).read_bytes().decode("utf-8")
    except OSError as error:
        raise ValidationError(f"cannot read {relative_path}: {error.strerror}") from error
    except UnicodeDecodeError as error:
        raise ValidationError(f"{relative_path} is not valid UTF-8") from error


def _anchor_set(root: Path, relative_path: str, cache: dict[str, set[str]]) -> set[str]:
    if relative_path not in cache:
        cache[relative_path] = set(emitted_anchors(read_markdown(root, relative_path)))
    return cache[relative_path]


def _check_target(
    root: Path, source: str, target: str, files: set[str], directories: set[str],
    cache: dict[str, set[str]],
) -> str | None:
    if target == INVALID_REFERENCE_TARGET:
        return "missing or malformed reference definition"
    path_part, fragment = split_local_target(target)
    if path_part == "":
        resolved = source
    else:
        resolved = resolve_local_path(source, target)
        if path_part.endswith("/"):
            if resolved not in directories:
                return "missing tracked directory"
            if fragment:
                return "directory link must not carry a fragment"
            return None
        if resolved not in files:
            return "missing tracked file"
    if not fragment:
        return None
    if not resolved.lower().endswith(MARKDOWN_SUFFIX):
        return "fragment target is not a Markdown file"
    # Fragments are compared byte-exactly: a decomposed or differently cased fragment does not
    # resolve on GitHub either, so normalizing here would accept a genuinely broken link.
    if fragment not in _anchor_set(root, resolved, cache):
        return "missing anchor"
    return None


def check_repository(root: Path | str, tracked: Sequence[str]) -> list[Finding]:
    """Return every link finding for the tracked Markdown files of one repository."""
    root = Path(root)
    files = set(tracked)
    directories = {
        parent.as_posix() for path in tracked for parent in PurePosixPath(path).parents
        if parent.as_posix() != "."
    }
    cache: dict[str, set[str]] = {}
    findings: list[Finding] = []
    for source in sorted(path for path in tracked if path.lower().endswith(MARKDOWN_SUFFIX)):
        _anchor_set(root, source, cache)
        text = read_markdown(root, source)
        for line, target in extract_links(text):
            if is_external(target):
                continue
            try:
                reason = _check_target(root, source, target, files, directories, cache)
            except ValidationError as error:
                reason = str(error)
            if reason is not None:
                findings.append(Finding(file=source, line=line, target=target, reason=reason))
    return findings


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def parse_arguments(arguments: Sequence[str] | None, description: str | None = None) -> None:
    """Accept no option: a validator always checks its own repository."""
    argparse.ArgumentParser(
        description=description or __doc__, allow_abbrev=False
    ).parse_args(arguments)


def run_validator(
    prefix: str, check: Callable[[Path, Sequence[str]], list[object]], repository: Path,
    stdout: TextIO, enumerate_tracked: Callable[[Path], list[str]],
) -> int:
    """Run one repository validator and map its findings to a process exit code.

    Shared by `check_repository_governance.py` so that both validators keep one
    Git enumeration, one argument contract, and one reporting format.
    """
    try:
        findings = check(repository, enumerate_tracked(repository))
    except ValidationError as error:
        print(f"{prefix}: ERROR {error}", file=sys.stderr)
        return 1
    for finding in findings:
        print(f"{prefix}: {finding}", file=stdout)
    print(f"{prefix}: {len(findings)} finding(s)", file=stdout)
    return 1 if findings else 0


def main(
    arguments: Sequence[str] | None = None, stdout: TextIO = sys.stdout,
    repository: Path = REPOSITORY_ROOT,
) -> int:
    parse_arguments(arguments)
    return run_validator("relative-links", check_repository, repository, stdout, tracked_files)


if __name__ == "__main__":
    raise SystemExit(main())
