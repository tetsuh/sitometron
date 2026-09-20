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
import re
import subprocess
import sys
import unicodedata
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from typing import TextIO

from markdown_it import MarkdownIt
from markdown_it.rules_block import reference as markdown_reference
from markdown_it.rules_inline import (
    autolink as markdown_autolink, backtick, image as markdown_image, link as markdown_link,
)

EXTERNAL_SCHEMES = frozenset({"http", "https", "mailto"})
MARKDOWN_SUFFIX = ".md"
SCHEME_PATTERN = re.compile(r"\A(\w[\w+.-]*):")
MAX_LABEL_DEPTH = 8
INVALID_REFERENCE_TARGET = "<invalid-reference>"
UNSUPPORTED_DESTINATION_TARGET = "<unsupported-destination>"
UNSUPPORTED_LABEL_TARGET = "<unsupported-label>"
MAX_DESTINATION_DEPTH = 8
LINK_WHITESPACE = " \t\n"
PERCENT_PATTERN = re.compile(r"%(..?|\Z)", re.DOTALL)
VALID_ESCAPE_PATTERN = re.compile(r"\A[0-9A-Fa-f]{2}\Z")
DRIVE_PATTERN = re.compile(r"\A[A-Za-z]:")
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

UNSUPPORTED_LABEL_DEPTH = -1


def _label_special_end(line: str, index: int, state) -> int | None:
    """Skip one escape or parser-owned code span inside a link label."""
    if line[index] == "\\":
        return index + 2
    if line[index] != "`" or state is None:
        return None
    saved = state.pos
    state.pos = index
    backtick(state, True)
    index, state.pos = state.pos, saved
    return index


def _label_end(line: str, start: int, state=None) -> int | None:
    if start >= len(line) or line[start] != "[":
        return None
    depth = 1
    index = start + 1
    while index < len(line):
        special_end = _label_special_end(line, index, state)
        if special_end is not None:
            index = special_end
            continue
        character = line[index]
        if character == "[":
            depth += 1
            if depth > MAX_LABEL_DEPTH:
                return UNSUPPORTED_LABEL_DEPTH
        elif character == "]":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return None

def _is_word_character(character: str) -> bool:
    """Report whether one character is a Unicode letter, number, or mark."""
    return unicodedata.category(character)[0] in KEPT_CATEGORIES


def _drop_emphasis(text: str) -> str:
    """Remove underscores used as emphasis delimiters, keeping intraword ones.

    A word character on both sides is intraword for every script, so `café_漢`
    and `snake_case` keep their underscore while `_emphasis_` loses both.
    Inline-code tokens bypass this function.
    """
    kept: list[str] = []
    for index, character in enumerate(text):
        if character != "_":
            kept.append(character)
            continue
        before = text[index - 1] if index else ""
        after = text[index + 1] if index + 1 < len(text) else ""
        if before and after and _is_word_character(before) and _is_word_character(after):
            kept.append(character)
    return "".join(kept)


def _visible_text(heading: str) -> str:
    """Apply the frozen visible-text policy to parsed inline content."""
    tokens = _markdown_parser().parseInline(_strip_closing_hashes(heading))[0].children or []
    def visible(items: Sequence) -> str:
        parts = []
        for token in items:
            if token.type == "code_inline":
                parts.append(token.content)
            elif token.type == "policy_finding":
                nested = _markdown_parser().parseInline(token.content)[0].children or []
                parts.append(visible(nested))
            elif token.type == "text":
                text = token.content
                text = re.sub(r"[`*~]", "", text)
                parts.append(_drop_emphasis(text))
            elif token.type in {"softbreak", "hardbreak"}:
                parts.append(" ")
            elif token.children:
                parts.append(visible(token.children))
        return "".join(parts)
    return visible(tokens)

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


def emitted_anchors(markdown: str) -> list[str]:
    """Return anchors for native CommonMark ATX headings in document order."""
    anchors: list[str] = []
    counts: dict[str, int] = {}
    tokens = _markdown_parser().parse(markdown)
    for index, token in enumerate(tokens):
        if token.type != "heading_open" or not token.markup.startswith("#"):
            continue
        if index + 1 >= len(tokens) or tokens[index + 1].type != "inline":
            raise ValidationError("Markdown heading has no inline content token")
        slug = slugify(tokens[index + 1].content)
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

def _destination_limit(text: str, start: int) -> int | None:
    """Bound bare-destination parentheses; native rules own all other syntax."""
    while start < len(text) and text[start] in LINK_WHITESPACE:
        start += 1
    if text[start:start + 1] == "<":
        return None
    depth = 1
    index = start
    while index < len(text):
        character = text[index]
        if character in LINK_WHITESPACE:
            break
        if character == "\\":
            index += 2
            continue
        depth += (character == "(") - (character == ")")
        if depth == 0:
            break
        if depth > MAX_DESTINATION_DEPTH:
            return index + 1
        index += 1
    return None


def _normalized_label(text: str) -> str:
    """Return one CommonMark-normalized reference label."""
    return " ".join(text.split()).casefold()


def parse_reference_definition(line: str) -> tuple[str, str] | None:
    """Parse one reference definition, honouring escaped brackets inside its label."""
    indent = len(line) - len(line.lstrip(" "))
    if indent > MAX_HEADING_INDENT or line[indent:indent + 1] != "[":
        return None
    end = _label_end(line, indent)
    if end is None or end == UNSUPPORTED_LABEL_DEPTH or line[end:end + 1] != ":":
        return None
    label = _normalized_label(line[indent + 1:end - 1])
    return (label, line[end + 1:].strip(" \t")) if label else None


def _policy_target(state, start: int) -> tuple[str, int, str] | None:
    """Reject only repository-specific unsupported/undefined link forms.

    Called at the parser's current inline position: code, HTML, and link titles
    have already been consumed by their own rules and cannot be scanned here.
    """
    text = state.src[:state.posMax]
    label_start = start + (text[start:start + 2] == "![")
    if text[label_start:label_start + 1] != "[":
        return None
    end = _label_end(text, label_start, state)
    if end == UNSUPPORTED_LABEL_DEPTH:
        run = label_start
        while text[run:run + 1] == "[":
            run += 1
        return UNSUPPORTED_LABEL_TARGET, run, ""
    if end is None:
        return None
    label = text[label_start + 1:end - 1]
    if text[end:end + 1] == "(":
        overflow = _destination_limit(text, end + 1)
        if overflow is not None:
            return UNSUPPORTED_DESTINATION_TARGET, overflow, label
    second = _label_end(text, end)
    if second == UNSUPPORTED_LABEL_DEPTH:
        run = end
        while text[run:run + 1] == "[":
            run += 1
        return UNSUPPORTED_LABEL_TARGET, run, label
    if second is not None:
        # Native parsing has first refusal; only unresolved explicit references
        # reach this fallback. Undefined shortcuts remain ordinary text.
        return INVALID_REFERENCE_TARGET, second, label
    return None


def _record_source_lines(tokens: Sequence, source: str, start: int) -> None:
    """Attach the source line to native link/image tokens emitted by one rule."""
    source_line = source.count("\n", 0, start)
    for token in tokens:
        if token.type in {"link_open", "image"}:
            token.meta.setdefault("source_line", source_line)


def _push_policy_finding(state, policy: tuple[str, int, str], start: int) -> None:
    """Emit one repository-policy token after native syntax declines a target."""
    token = state.push("policy_finding", "", 0)
    token.attrs = {"target": policy[0]}
    token.content = policy[2]
    token.meta["source_line"] = state.src.count("\n", 0, start)
    state.pos = policy[1]


def _link_rule(native):
    prefix = {markdown_image: "![", markdown_link: "[", markdown_autolink: "<"}[native]
    def parse(state, silent):
        start, first = state.pos, len(state.tokens)
        if not state.src.startswith(prefix, start):
            return False
        # Depth failures must be reported even when the library's own nesting
        # limit would otherwise turn a link into ordinary text.
        policy = _policy_target(state, start)
        limited = policy is not None and policy[0] != INVALID_REFERENCE_TARGET
        if not limited and native(state, silent):
            if not silent:
                _record_source_lines(state.tokens[first:], state.src, start)
            return True
        if policy is None or silent:
            return False
        _push_policy_finding(state, policy, start)
        return True
    return parse


def _invalid_reference_end(state, start: int, end: int) -> int | None:
    """Find a rejected definition within its native block boundaries."""
    next_line = start + 1
    terminators = state.md.block.ruler.getRules("reference")
    previous_parent = state.parentType
    state.parentType = "reference"
    try:
        while True:
            candidate = state.getLines(start, next_line, state.blkIndent, False).strip()
            if parse_reference_definition(candidate) is not None:
                return next_line
            if next_line >= end or state.isEmpty(next_line):
                return None
            if (state.sCount[next_line] - state.blkIndent > MAX_HEADING_INDENT or
                    state.sCount[next_line] < 0):
                next_line += 1
                continue
            if any(rule(state, next_line, end, True) for rule in terminators):
                return None
            next_line += 1
    finally:
        state.parentType = previous_parent


def _markdown_parser() -> MarkdownIt:
    """Parse structure without rendering, URL normalization, or scheme filtering."""
    parser = MarkdownIt("commonmark", {"inline_definitions": True})
    def bounded(native):
        def checked(state, *args):
            if state.level >= parser.options.maxNesting:
                raise ValidationError("Markdown nesting exceeds the parser limit")
            return native(state, *args)
        return checked

    # The library otherwise skips content silently when its nesting limit is hit.
    parser.block.tokenize = bounded(parser.block.tokenize)
    parser.inline.tokenize = bounded(parser.inline.tokenize)
    parser.inline.skipToken = bounded(parser.inline.skipToken)
    parser.normalizeLink = lambda value: value
    parser.validateLink = lambda value: True
    original_destination = parser.helpers.parseLinkDestination

    def raw_destination(text, start, maximum):
        result = original_destination(text, start, maximum)
        if result.ok:
            raw = text[start:result.pos]
            result.str = raw[1:-1] if raw.startswith("<") else raw
        return result

    # Helpers are copied per instance: never modify markdown-it's shared module.
    parser.helpers = SimpleNamespace(**vars(parser.helpers))
    parser.helpers.parseLinkDestination = raw_destination
    parser.inline.ruler.at("link", _link_rule(markdown_link))
    parser.inline.ruler.at("image", _link_rule(markdown_image))
    parser.inline.ruler.at("autolink", _link_rule(markdown_autolink))

    def reference(state, start, end, silent):
        if markdown_reference(state, start, end, silent):
            return True
        if state.is_code_block(start):
            return False
        definition_end = _invalid_reference_end(state, start, end)
        if definition_end is None or silent:
            return False
        token = state.push("definition", "", 0)
        token.meta["url"] = INVALID_REFERENCE_TARGET
        token.map = [start, definition_end]
        state.line = definition_end
        return True
    parser.block.ruler.at("reference", reference)
    return parser


def _token_target(token) -> str | None:
    """Return the raw destination attribute for one parser link token."""
    attribute = {"link_open": "href", "image": "src", "policy_finding": "target"}.get(token.type)
    return token.attrGet(attribute) if attribute is not None else None


def _collect_link_tokens(tokens: Sequence, base: int) -> list[tuple[int, str]]:
    """Collect targets recursively while preserving image-child line offsets."""
    links: list[tuple[int, str]] = []
    for token in tokens:
        source_line = token.meta.get("source_line", 0)
        target = _token_target(token)
        if target:
            links.append((base + source_line, target))
        if token.children:
            child_base = base + source_line if token.type == "image" else base
            links.extend(_collect_link_tokens(token.children, child_base))
    return links


def extract_links(markdown: str) -> list[tuple[int, str]]:
    """Extract native links/images and every definition with their source lines."""
    links: list[tuple[int, str]] = []
    for token in _markdown_parser().parse(markdown):
        if token.type == "definition":
            links.append((token.map[0] + 1, token.meta["url"]))
        elif token.type == "inline":
            links.extend(_collect_link_tokens(token.children or [], token.map[0] + 1))
    return sorted(links)


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
    if target == UNSUPPORTED_DESTINATION_TARGET:
        return "link destination nests deeper than the validator supports"
    if target == UNSUPPORTED_LABEL_TARGET:
        return "link label nests deeper than the validator supports"
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
