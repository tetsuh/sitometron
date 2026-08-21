#!/usr/bin/env python3
"""Run the frozen Phase 0A clang-tidy qualification set."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections.abc import Callable, Sequence
from pathlib import Path, PurePosixPath
from typing import Any

EXPECTED_SOURCE_COUNT = 12
EXPECTED_VERSION = "18.1.3"
SOURCE_ROOTS = frozenset({"apps", "src", "tests"})
Runner = Callable[..., subprocess.CompletedProcess[str]]


class QualificationError(RuntimeError):
    """A fail-closed qualification error."""


def require_application(application: Path) -> Path:
    if not application.is_absolute():
        raise QualificationError("clang-tidy application path must be absolute")
    resolved = application.resolve()
    if not resolved.is_file():
        raise QualificationError(
            f"clang-tidy application is not a regular file: {resolved}"
        )
    return resolved


def require_version(application: Path, runner: Runner = subprocess.run) -> None:
    result = runner(
        [str(application), "--version"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise QualificationError(
            f"clang-tidy version command failed with exit {result.returncode}"
        )
    version_pattern = rf"\bLLVM version {re.escape(EXPECTED_VERSION)}(?:\s|$)"
    if re.search(version_pattern, result.stdout) is None:
        raise QualificationError(
            f"required clang-tidy version is {EXPECTED_VERSION}; "
            f"reported output was {result.stdout.strip()!r}"
        )
    print(result.stdout.strip())


def read_compile_database(path: Path) -> list[dict[str, Any]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise QualificationError(f"cannot read compile_commands.json: {error}") from error
    if not isinstance(value, list):
        raise QualificationError("compile_commands.json root must be an array")
    return value


def tracked_paths(repository_root: Path) -> list[str]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repository_root),
            "ls-files",
            "-z",
            "--",
            "apps",
            "src",
            "tests",
        ],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise QualificationError(
            f"Git source enumeration failed with exit {result.returncode}: {message}"
        )
    try:
        return [item for item in result.stdout.decode("utf-8").split("\0") if item]
    except UnicodeDecodeError as error:
        raise QualificationError("Git source paths are not valid UTF-8") from error


def _compile_source(entry: dict[str, Any]) -> Path:
    if not isinstance(entry, dict):
        raise QualificationError("compile_commands.json entries must be objects")
    directory = entry.get("directory")
    source = entry.get("file")
    if not isinstance(directory, str) or not directory:
        raise QualificationError("compile_commands.json entry has no directory")
    if not isinstance(source, str) or not source:
        raise QualificationError("compile_commands.json entry has no file")
    directory_path = Path(directory)
    if not directory_path.is_absolute():
        raise QualificationError("compile_commands.json directory must be absolute")
    return (directory_path / source).resolve()


def select_sources(
    repository_root: Path,
    tracked: Sequence[str],
    database: Sequence[dict[str, Any]],
) -> list[Path]:
    repository_root = repository_root.resolve()
    compiled = {_compile_source(entry) for entry in database}
    selected: set[Path] = set()
    for raw_path in tracked:
        path = PurePosixPath(raw_path)
        if (
            path.is_absolute()
            or "\\" in raw_path
            or ".." in path.parts
            or not path.parts
            or path.parts[0] not in SOURCE_ROOTS
            or path.suffix != ".cpp"
        ):
            continue
        candidate = (repository_root / Path(*path.parts)).resolve()
        try:
            candidate.relative_to(repository_root)
        except ValueError:
            continue
        if candidate in compiled:
            if not candidate.is_file():
                raise QualificationError(f"tracked source is not a regular file: {raw_path}")
            selected.add(candidate)
    if not selected:
        raise QualificationError(
            "no tracked apps/src/tests C++ sources matched compile_commands.json"
        )
    return sorted(selected, key=lambda item: item.relative_to(repository_root).as_posix())


def require_source_count(sources: Sequence[Path]) -> None:
    if len(sources) != EXPECTED_SOURCE_COUNT:
        raise QualificationError(
            f"expected {EXPECTED_SOURCE_COUNT} clang-tidy sources; found {len(sources)}"
        )


def run_analysis(
    application: Path,
    build_directory: Path,
    sources: Sequence[Path],
    runner: Runner = subprocess.run,
) -> None:
    if not sources:
        raise QualificationError("clang-tidy source selection is empty")
    arguments = [
        str(application),
        "-p",
        str(build_directory),
        "--warnings-as-errors=*",
        *(str(source) for source in sources),
    ]
    result = runner(arguments, capture_output=True, text=True, check=False)
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(
            result.stderr,
            end="" if result.stderr.endswith("\n") else "\n",
            file=sys.stderr,
        )
    if result.returncode != 0:
        raise QualificationError(
            f"clang-tidy qualification failed with exit {result.returncode}"
        )


def parse_arguments(arguments: Sequence[str] | None) -> argparse.Namespace:
    repository_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-tidy", required=True, type=Path)
    parser.add_argument("--repository-root", type=Path, default=repository_root)
    parser.add_argument(
        "--compile-commands",
        type=Path,
        default=repository_root / "build" / "dev-linux" / "compile_commands.json",
    )
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    try:
        repository_root = options.repository_root.resolve()
        compile_commands = options.compile_commands.resolve()
        application = require_application(options.clang_tidy)
        database = read_compile_database(compile_commands)
        sources = select_sources(
            repository_root, tracked_paths(repository_root), database
        )
        require_source_count(sources)
        require_version(application)
        print(f"clang-tidy source count: {len(sources)}")
        run_analysis(application, compile_commands.parent, sources)
    except QualificationError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
