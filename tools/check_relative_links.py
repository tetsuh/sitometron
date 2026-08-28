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
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TextIO

EXTERNAL_SCHEMES = frozenset({"http", "https", "mailto"})
MARKDOWN_SUFFIX = ".md"
SCHEME_PATTERN = re.compile(r"\A([A-Za-z][A-Za-z0-9+.-]*):")
FENCE_PATTERN = re.compile(r"\A {0,3}(```|~~~)")
HEADING_PATTERN = re.compile(r"\A {0,3}(#{1,6})(?:[ \t]+(.*))?\Z")
LINK_PATTERN = re.compile(r"(?<!\\)!?\[(?:[^\[\]\\]|\\.)*\]\(\s*([^()\s]*)[^()]*\)")
LABEL_LINK_PATTERN = re.compile(r"!?\[((?:[^\[\]\\]|\\.)*)\]\([^()]*\)")
HTML_TAG_PATTERN = re.compile(r"<[^>]*>")
CLOSING_HASHES_PATTERN = re.compile(r"[ \t]+#+[ \t]*\Z")
PERCENT_PATTERN = re.compile(r"%(..?|\Z)", re.DOTALL)
VALID_ESCAPE_PATTERN = re.compile(r"\A[0-9A-Fa-f]{2}\Z")
DRIVE_PATTERN = re.compile(r"\A[A-Za-z]:")
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
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in decoded):
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
    components = path.rstrip("/").split("/") if path != "/" else [""]
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


def _visible_text(heading: str) -> str:
    text = CLOSING_HASHES_PATTERN.sub("", heading)
    text = LABEL_LINK_PATTERN.sub(lambda match: match.group(1), text)
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


def _content_lines(markdown: str) -> list[str]:
    lines: list[str] = []
    fenced = False
    for line in markdown.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if FENCE_PATTERN.match(line):
            fenced = not fenced
            continue
        if not fenced:
            lines.append(line)
        else:
            lines.append("")
    return lines


def emitted_anchors(markdown: str) -> list[str]:
    """Return every anchor emitted by the ATX headings of one document, in order."""
    anchors: list[str] = []
    counts: dict[str, int] = {}
    for line in _content_lines(markdown):
        match = HEADING_PATTERN.match(line)
        if match is None:
            continue
        raw = match.group(2)
        if raw is None or not raw.strip():
            continue
        slug = slugify(raw)
        seen = counts.get(slug, 0)
        counts[slug] = seen + 1
        anchors.append(slug if seen == 0 else f"{slug}-{seen}")
    return anchors


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


def extract_links(markdown: str) -> list[tuple[int, str]]:
    """Return every (line number, raw target) link outside fenced code."""
    links: list[tuple[int, str]] = []
    for number, line in enumerate(_content_lines(markdown), 1):
        for target in LINK_PATTERN.findall(line):
            if target:
                links.append((number, target))
    return links


def _anchor_set(root: Path, relative_path: str, cache: dict[str, set[str]]) -> set[str]:
    if relative_path not in cache:
        text = (root / relative_path).read_text(encoding="utf-8")
        cache[relative_path] = set(emitted_anchors(text))
    return cache[relative_path]


def _check_target(
    root: Path, source: str, target: str, files: set[str], directories: set[str],
    cache: dict[str, set[str]],
) -> str | None:
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
    if unicodedata.normalize("NFC", fragment).casefold() not in _anchor_set(root, resolved, cache):
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
        text = (root / source).read_text(encoding="utf-8")
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


def parse_arguments(arguments: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[1])
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None, stdout: TextIO = sys.stdout) -> int:
    options = parse_arguments(arguments)
    try:
        findings = check_repository(options.repository, tracked_files(options.repository))
    except ValidationError as error:
        print(f"relative-links: ERROR {error}", file=sys.stderr)
        return 1
    for finding in findings:
        print(f"relative-links: {finding}", file=stdout)
    print(f"relative-links: {len(findings)} finding(s)", file=stdout)
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
