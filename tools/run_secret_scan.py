#!/usr/bin/env python3
"""Run the pinned Phase 0A Gitleaks secret scan against one exact Git head.

The runner asserts the expected head, materializes only tracked regular-file
blobs into a run-owned tree, validates the repository-owned pins and
configuration from that tree, acquires the pinned Gitleaks release, qualifies
the detector with ephemeral canary probes, scans the materialized tree, and
removes every run-owned file. It prints only bounded stage and category
diagnostics and never echoes scanner output, redirect targets, or probe values.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import stat
import subprocess
import sys
import tempfile
import tomllib
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, TextIO

TOOLS_DIRECTORY = Path(__file__).resolve().parent
if str(TOOLS_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIRECTORY))

import acquire_gitleaks  # noqa: E402

EXIT_CLEAN = 0
EXIT_FINDING = 1
EXIT_FAILURE = 2
CONFIGURATION_FILE = ".gitleaks.toml"
CANARY_RULE_ID = "sitometron-synthetic-canary"
CANARY_KEYWORD = "SITOMETRON_CANARY_"
CANARY_REGEX = CANARY_KEYWORD + "[0-9a-f]{32}"
CANARY_SEED_FRAGMENTS = ("sitometron", "phase-0a", "synthetic", "canary")
HEAD_PATTERN = re.compile(r"\A[0-9a-f]{40}\Z")
RECORD_PATTERN = re.compile(rb"\A([0-7]{6}) (blob|commit|tree) ([0-9a-f]{40})\t(.*)\Z", re.DOTALL)
INCLUDED_MODES = frozenset({"100644", "100755"})
EXCLUDED_MODES = frozenset({"120000", "160000"})
PROBE_FILE = "probe.txt"
FINDING_FLAG_EXIT = 1
GITLEAKS_OPTIONS = ("--no-banner", "--no-color", "--redact=100", "--ignore-gitleaks-allow")
ProcessRunner = acquire_gitleaks.ProcessRunner


class ScanError(RuntimeError):
    def __init__(self, category: str, message: str) -> None:
        super().__init__(message)
        self.category = category


@dataclass
class Progress:
    stage: str = "arguments"


@dataclass(frozen=True)
class TrackedBlob:
    mode: str
    oid: str
    path: str


def synthetic_canary() -> str:
    digest = hashlib.sha256("-".join(CANARY_SEED_FRAGMENTS).encode("ascii")).hexdigest()
    return CANARY_KEYWORD + digest[:32]


def near_match() -> str:
    return synthetic_canary()[:-1] + "z"


def parse_arguments(arguments: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, add_help=True)
    parser.add_argument("--repository", required=True, type=Path)
    parser.add_argument("--expected-head", required=True)
    options = parser.parse_args(arguments)
    if HEAD_PATTERN.match(options.expected_head) is None:
        parser.error("--expected-head must be exactly 40 lowercase hexadecimal characters")
    return options


def _run(run_process: ProcessRunner, arguments: list[str], category: str, **options: Any) -> Any:
    try:
        return run_process(arguments, capture_output=True, check=False, **options)
    except OSError as error:
        raise ScanError(category, f"{arguments[0]} process could not start: {error.strerror}") from error


def _git(repository: Path, run_process: ProcessRunner, *command: str) -> bytes:
    result = _run(run_process, ["git", "-C", str(repository), *command], "policy-failure")
    if result.returncode != 0:
        raise ScanError("policy-failure", f"git {command[0]} failed with exit {result.returncode}")
    return result.stdout if isinstance(result.stdout, bytes) else str(result.stdout).encode("utf-8")


def assert_head(repository: Path, expected_head: str, run_process: ProcessRunner) -> None:
    actual = _git(repository, run_process, "rev-parse", "--verify", "HEAD^{commit}").decode("ascii", "replace")
    if actual.strip() != expected_head:
        raise ScanError("policy-failure", "repository HEAD does not equal the expected event head")


def _validate_path(raw: bytes) -> str:
    try:
        path = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ScanError("policy-failure", "tracked path is not valid UTF-8") from error
    if not path or "\\" in path or "\0" in path or path.startswith("/") or re.match(r"\A[A-Za-z]:", path):
        raise ScanError("policy-failure", "tracked path is empty, absolute, or not POSIX-relative")
    parts = path.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise ScanError("policy-failure", "tracked path contains an empty, dot, or dot-dot component")
    return path


def enumerate_tracked_blobs(repository: Path, run_process: ProcessRunner) -> list[TrackedBlob]:
    output = _git(repository, run_process, "ls-tree", "-r", "-z", "HEAD")
    blobs: list[TrackedBlob] = []
    seen: set[str] = set()
    for record in output.split(b"\0"):
        if not record:
            continue
        match = RECORD_PATTERN.match(record)
        if match is None:
            raise ScanError("policy-failure", "tracked-object record is malformed")
        mode, kind, oid = (match.group(index).decode("ascii") for index in (1, 2, 3))
        path = _validate_path(match.group(4))
        if path in seen:
            raise ScanError("policy-failure", "tracked path is listed twice")
        seen.add(path)
        if mode in EXCLUDED_MODES:
            continue
        if mode not in INCLUDED_MODES or kind != "blob":
            raise ScanError("policy-failure", f"tracked object has unexpected mode/type {mode} {kind}")
        blobs.append(TrackedBlob(mode=mode, oid=oid, path=path))
    if not blobs:
        raise ScanError("policy-failure", "no tracked regular-file blobs were enumerated")
    return blobs


def _is_link(path: Path) -> bool:
    try:
        status = os.lstat(path)
    except FileNotFoundError:
        return False
    attributes = getattr(status, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return stat.S_ISLNK(status.st_mode) or bool(attributes & reparse)


def materialize_blobs(
    repository: Path, blobs: Sequence[TrackedBlob], scan_root: Path, run_process: ProcessRunner
) -> int:
    for blob in blobs:
        result = _run(run_process, ["git", "-C", str(repository), "cat-file", "blob", blob.oid], "policy-failure")
        if result.returncode != 0:
            raise ScanError("policy-failure", f"git cat-file failed with exit {result.returncode}")
        data = result.stdout if isinstance(result.stdout, bytes) else str(result.stdout).encode("utf-8")
        if hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest() != blob.oid:
            raise ScanError("policy-failure", "materialized blob does not match its enumerated object")
        parts = PurePosixPath(blob.path).parts
        directory = scan_root
        for part in parts[:-1]:
            directory = directory / part
            if _is_link(directory):
                raise ScanError("policy-failure", "scan tree ancestor is a symbolic link or reparse point")
            directory.mkdir(exist_ok=True)
        target = directory / parts[-1]
        if _is_link(target):
            raise ScanError("policy-failure", "scan tree target is a symbolic link or reparse point")
        try:
            descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except OSError as error:
            raise ScanError("policy-failure", f"scan tree path already exists or cannot be created: {error.strerror}") from error
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
    return len(blobs)


def validate_configuration(path: Path) -> None:
    try:
        document = tomllib.loads(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise ScanError("policy-failure", f"{CONFIGURATION_FILE} is unreadable or malformed") from error
    if set(document) != {"title", "extend", "rules"}:
        raise ScanError("policy-failure", f"{CONFIGURATION_FILE} must contain only title, extend, and rules")
    if not isinstance(document["title"], str) or not document["title"].strip():
        raise ScanError("policy-failure", f"{CONFIGURATION_FILE} title must be a non-empty string")
    if document["extend"] != {"useDefault": True}:
        raise ScanError("policy-failure", f"{CONFIGURATION_FILE} must extend only the built-in default rules")
    rules = document["rules"]
    if not isinstance(rules, list) or len(rules) != 1 or not isinstance(rules[0], dict):
        raise ScanError("policy-failure", f"{CONFIGURATION_FILE} must define exactly one rule")
    rule = dict(rules[0])
    description = rule.pop("description", "")
    expected = {"id": CANARY_RULE_ID, "keywords": [CANARY_KEYWORD], "regex": CANARY_REGEX}
    if rule != expected or not isinstance(description, str):
        raise ScanError("policy-failure", f"{CONFIGURATION_FILE} canary rule differs from the frozen rule")


def _write_probe(directory: Path, line: str, ignore_lines: Sequence[str] = ()) -> Path:
    directory.mkdir()
    probe = directory / PROBE_FILE
    probe.write_text(line + "\n", encoding="utf-8")
    if ignore_lines:
        (directory / ".gitleaksignore").write_text("".join(f"{item}\n" for item in ignore_lines), encoding="utf-8")
    return directory


def _scan(
    application: Path, target: Path, configuration: Path, ignore_file: Path, workspace: Path,
    run_process: ProcessRunner,
) -> int:
    if ignore_file.stat().st_size != 0:
        raise ScanError("policy-failure", "run-owned ignore file is not empty")
    arguments = [
        str(application), "dir", str(target), *GITLEAKS_OPTIONS, "--gitleaks-ignore-path", str(ignore_file),
        "--config", str(configuration), "--exit-code", str(FINDING_FLAG_EXIT),
    ]
    result = _run(run_process, arguments, "tool-failure", cwd=str(workspace))
    return int(result.returncode)


def _require_exit(stage: str, observed: int, expected: int, out: TextIO) -> None:
    if observed not in (EXIT_CLEAN, FINDING_FLAG_EXIT):
        raise ScanError("tool-failure", f"stage {stage} exited {observed}")
    if observed != expected:
        raise ScanError("probe-failure", f"stage {stage} exited {observed}; expected {expected}")
    print(f"secret-scan: stage={stage} exit={observed} ok", file=out)


def execute(
    options: argparse.Namespace, workspace: Path, run_process: ProcessRunner, fetch: Any, out: TextIO,
    progress: Progress,
) -> int:
    progress.stage = "head-assertion"
    try:
        repository = options.repository.resolve(strict=True)
    except OSError as error:
        raise ScanError("policy-failure", "repository path cannot be resolved") from error
    if not repository.is_dir():
        raise ScanError("policy-failure", "repository path is not a directory")
    assert_head(repository, options.expected_head, run_process)
    print(f"secret-scan: stage=head-assertion head={options.expected_head} ok", file=out)
    progress.stage = "materialization"
    blobs = enumerate_tracked_blobs(repository, run_process)
    scan_tree = workspace / "scan-tree"
    scan_tree.mkdir()
    count = materialize_blobs(repository, blobs, scan_tree, run_process)
    print(f"secret-scan: stage=materialization blobs={count} ok", file=out)
    progress.stage = "configuration"
    try:
        pin = acquire_gitleaks.read_tool_pin(scan_tree / "tools")
    except acquire_gitleaks.AcquisitionError as error:
        raise ScanError("policy-failure", str(error)) from error
    configuration = scan_tree / CONFIGURATION_FILE
    validate_configuration(configuration)
    print(f"secret-scan: stage=configuration version={pin.version} ok", file=out)
    progress.stage = "acquisition"
    tool_directory = workspace / "tool"
    tool_directory.mkdir()
    try:
        application = acquire_gitleaks.acquire(pin, tool_directory, fetch=fetch, run_process=run_process)
    except acquire_gitleaks.AcquisitionError as error:
        raise ScanError("acquisition-failure", str(error)) from error
    print(f"secret-scan: stage=acquisition version={pin.version} ok", file=out)
    ignore_file = workspace / "empty.gitleaksignore"
    ignore_file.touch(mode=0o600, exist_ok=False)
    canary = synthetic_canary()
    probe_line = f'probe = "{canary}"'
    ignore_probe = workspace / "probe-ignore-state"
    fingerprints = [
        f"{ignore_probe / PROBE_FILE}:{CANARY_RULE_ID}:1",
        f"{PROBE_FILE}:{CANARY_RULE_ID}:1",
    ]
    probes = [
        ("probe-positive", _write_probe(workspace / "probe-positive", probe_line), FINDING_FLAG_EXIT),
        ("probe-near-match", _write_probe(workspace / "probe-near-match", f'probe = "{near_match()}"'), EXIT_CLEAN),
        ("probe-inline-allow", _write_probe(workspace / "probe-inline-allow", f"{probe_line}  # gitleaks:allow"), FINDING_FLAG_EXIT),
        ("probe-ignore-state", _write_probe(ignore_probe, probe_line, fingerprints), FINDING_FLAG_EXIT),
    ]
    for stage, target, expected in probes:
        progress.stage = stage
        observed = _scan(application, target, configuration, ignore_file, workspace, run_process)
        _require_exit(stage, observed, expected, out)
    progress.stage = "repository"
    observed = _scan(application, scan_tree, configuration, ignore_file, workspace, run_process)
    if observed == EXIT_CLEAN:
        print("secret-scan: stage=repository exit=0 category=clean", file=out)
        return EXIT_CLEAN
    if observed == FINDING_FLAG_EXIT:
        raise ScanError("finding", "repository scan reported at least one finding")
    raise ScanError("tool-failure", f"repository scan exited {observed}")


def run(
    arguments: Sequence[str] | None = None,
    *,
    run_process: ProcessRunner = subprocess.run,
    fetch: Any = acquire_gitleaks.open_https,
    workspace_parent: Path | None = None,
    stdout: TextIO = sys.stdout,
    stderr: TextIO = sys.stderr,
) -> int:
    options = parse_arguments(arguments)
    progress = Progress()
    try:
        with tempfile.TemporaryDirectory(prefix="sitometron-secret-scan-", dir=workspace_parent) as name:
            return execute(options, Path(name).resolve(), run_process, fetch, stdout, progress)
    except ScanError as error:
        print(
            f"secret-scan: stage={progress.stage} category={error.category} detail={str(error)[:200]}",
            file=stderr,
        )
        return EXIT_FINDING if error.category == "finding" else EXIT_FAILURE


def main(arguments: Sequence[str] | None = None) -> int:
    return run(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
