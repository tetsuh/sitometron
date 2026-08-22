import hashlib
import importlib.util
import io
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HELPER = REPOSITORY_ROOT / "tools" / "run_secret_scan.py"
HEAD = "0123456789abcdef0123456789abcdef01234567"
OTHER_HEAD = "fedcba9876543210fedcba9876543210fedcba98"
CANARY_PATTERN = re.compile(r"SITOMETRON_CANARY_[0-9a-f]{32}")
PROBE_NAMES = ["probe-positive", "probe-near-match", "probe-inline-allow", "probe-ignore-state"]
VALID_CONFIG = """title = "Sitometron secret scan"

[extend]
useDefault = true

[[rules]]
id = "sitometron-synthetic-canary"
description = "Deterministic non-secret canary used only by the Phase 0A probes."
keywords = ["SITOMETRON_CANARY_"]
regex = '''SITOMETRON_CANARY_[0-9a-f]{32}'''
"""


def load_module(name: str, path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FIXTURES = load_module("acquire_fixtures", Path(__file__).with_name("test_acquire_gitleaks.py"))


def blob_oid(data: bytes) -> str:
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def record(mode: str, kind: str, oid: str, path: bytes) -> bytes:
    return f"{mode} {kind} {oid}\t".encode("ascii") + path


class FakeRepository:
    def __init__(self, head: str = HEAD) -> None:
        self.head = head
        self.files: dict[str, tuple[str, bytes]] = {}
        self.extra_records: list[bytes] = []
        self.blob_override: dict[str, bytes] = {}
        self.cat_file_failures: set[str] = set()
        digest = hashlib.sha256(FIXTURES.valid_archive()).hexdigest()
        self.add("tools/gitleaks-tool-version.txt", b"8.30.1\n")
        self.add("tools/gitleaks-linux-x64.sha256", f"{digest}  {FIXTURES.ARCHIVE_NAME}\n".encode())
        self.add(".gitleaks.toml", VALID_CONFIG.encode())
        self.add("README.md", b"# fixture\n")
        self.add("src/core/value.cpp", b"int value;\n")
        self.add("tools/script.sh", b"#!/bin/sh\n", mode="100755")

    def add(self, path: str, data: bytes, mode: str = "100644") -> None:
        self.files[path] = (mode, data)

    def regular_paths(self) -> list[str]:
        return sorted(path for path, (mode, _) in self.files.items() if mode in {"100644", "100755"})

    def ls_tree(self) -> bytes:
        records = [
            record(mode, "commit" if mode == "160000" else "blob", blob_oid(data), path.encode())
            for path, (mode, data) in self.files.items()
        ]
        records.extend(self.extra_records)
        return b"".join(item + b"\0" for item in records)

    def cat_file(self, oid: str) -> bytes | None:
        if oid in self.cat_file_failures:
            return None
        if oid in self.blob_override:
            return self.blob_override[oid]
        for _, data in self.files.values():
            if blob_oid(data) == oid:
                return data
        return None


class FakeProcesses:
    def __init__(self, repository: FakeRepository, version: bytes = b"8.30.1\n") -> None:
        self.repository = repository
        self.version = version
        self.exit_overrides: dict[str, int] = {}
        self.start_failures: set[str] = set()
        self.calls: list[tuple[list[str], dict[str, object]]] = []
        self.scan_snapshots: dict[str, list[str]] = {}
        self.ignore_sizes: list[int] = []

    def dir_calls(self) -> list[list[str]]:
        return [args for args, _ in self.calls if len(args) > 1 and args[1] == "dir"]

    def __call__(self, args: list[str], **kwargs: object) -> subprocess.CompletedProcess:
        args = list(args)
        self.calls.append((args, kwargs))
        if args[0] == "git" and args[1] == "-C":
            return self._git(args, args[3:])
        if args[1] == "version":
            return FIXTURES.completed(args, 0, self.version)
        if args[1] == "dir":
            return self._gitleaks(args)
        raise AssertionError(f"unexpected process: {args}")

    def _git(self, args: list[str], command: list[str]) -> subprocess.CompletedProcess:
        if command == ["rev-parse", "--verify", "HEAD^{commit}"]:
            return FIXTURES.completed(args, 0, f"{self.repository.head}\n".encode("ascii"))
        if command == ["ls-tree", "-r", "-z", "HEAD"]:
            return FIXTURES.completed(args, 0, self.repository.ls_tree())
        if command[:2] == ["cat-file", "blob"] and len(command) == 3:
            data = self.repository.cat_file(command[2])
            return FIXTURES.completed(args, 128 if data is None else 0, data or b"")
        raise AssertionError(f"unexpected git command: {command}")

    def _gitleaks(self, args: list[str]) -> subprocess.CompletedProcess:
        target = Path(args[2])
        flags = args[3:]
        if target.name in self.start_failures:
            raise OSError("cannot start gitleaks")
        if target.name in self.exit_overrides:
            return FIXTURES.completed(args, self.exit_overrides[target.name])
        if "--gitleaks-ignore-path" in flags:
            ignore_path = Path(flags[flags.index("--gitleaks-ignore-path") + 1])
            self.ignore_sizes.append(ignore_path.stat().st_size)
        else:
            ignore_path = target / ".gitleaksignore"
        ignored = set(ignore_path.read_text().split()) if ignore_path.is_file() else set()
        files = sorted(path for path in target.rglob("*") if path.is_file())
        self.scan_snapshots[target.name] = [path.relative_to(target).as_posix() for path in files]
        findings = 0
        for path in files:
            for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                if CANARY_PATTERN.search(line) is None:
                    continue
                if "gitleaks:allow" in line and "--ignore-gitleaks-allow" not in flags:
                    continue
                fingerprints = {
                    f"{path}:sitometron-synthetic-canary:{number}",
                    f"{path.relative_to(target).as_posix()}:sitometron-synthetic-canary:{number}",
                }
                if fingerprints & ignored:
                    continue
                findings += 1
        exit_code = int(flags[flags.index("--exit-code") + 1]) if findings else 0
        return FIXTURES.completed(args, exit_code)


class RunSecretScanPresenceTest(unittest.TestCase):
    def test_helper_exists(self) -> None:
        self.assertTrue(HELPER.is_file(), "tools/run_secret_scan.py is absent")


@unittest.skipUnless(HELPER.is_file(), "secret-scan helper is not implemented")
class RunSecretScanTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name).resolve()
        self.module = load_module("run_secret_scan", HELPER)
        self.error = self.module.ScanError
        self.repository_path = self.root / "repository"
        self.repository_path.mkdir()
        self.workspaces = self.root / "workspaces"
        self.workspaces.mkdir()
        self.repository = FakeRepository()
        self.processes = FakeProcesses(self.repository)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def reset(self) -> None:
        self.repository = FakeRepository()
        self.processes = FakeProcesses(self.repository)

    def transport(self):  # type: ignore[no-untyped-def]
        return FIXTURES.FakeTransport({
            FIXTURES.INITIAL_URL: FIXTURES.redirect(FIXTURES.SIGNED_URL),
            FIXTURES.SIGNED_URL: FIXTURES.ok_response(FIXTURES.valid_archive()),
        })

    def run_scan(self, head: str = HEAD, repository: Path | None = None) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        self.transport_used = self.transport()
        code = self.module.run(
            ["--repository", str(repository or self.repository_path), "--expected-head", head],
            run_process=self.processes,
            fetch=self.transport_used,
            workspace_parent=self.workspaces,
            stdout=out,
            stderr=err,
        )
        text = out.getvalue() + err.getvalue()
        self.assertEqual(list(self.workspaces.iterdir()), [], "workspace must be removed")
        self.assertNotIn(self.module.synthetic_canary(), text)
        self.assertNotIn("SIGNED-QUERY-SENTINEL", text)
        self.assertNotIn("sig=", text)
        return code, out.getvalue(), err.getvalue()

    def test_canary_values_are_deterministic_and_uncommitted(self) -> None:
        canary = self.module.synthetic_canary()
        self.assertRegex(canary, r"^SITOMETRON_CANARY_[0-9a-f]{32}$")
        self.assertEqual(canary, self.module.synthetic_canary())
        self.assertIsNone(CANARY_PATTERN.search(self.module.near_match()))
        self.assertEqual(len(self.module.near_match()), len(canary))
        for path in [HELPER, Path(__file__), REPOSITORY_ROOT / ".gitleaks.toml"]:
            if path.is_file():
                self.assertIsNone(CANARY_PATTERN.search(path.read_text(encoding="utf-8")), path)

    def test_rejects_malformed_command_lines(self) -> None:
        good = ["--repository", str(self.repository_path), "--expected-head", HEAD]
        self.module.parse_arguments(good)
        for arguments in [
            [], good[:2], good[2:], [*good, "extra"], [*good, "--verbose"],
            ["--repository", str(self.repository_path), "--expected-head", HEAD.upper()],
            ["--repository", str(self.repository_path), "--expected-head", HEAD[:39]],
            ["--repository", str(self.repository_path), "--expected-head", HEAD + "0"],
        ]:
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    self.module.parse_arguments(arguments)
        code, _, err = self.run_scan(repository=self.root / "absent")
        self.assertEqual(code, self.module.EXIT_FAILURE)
        self.assertIn("category=policy-failure", err)

    def test_head_mismatch_stops_before_acquisition(self) -> None:
        code, _, err = self.run_scan(head=OTHER_HEAD)
        self.assertEqual(code, self.module.EXIT_FAILURE)
        self.assertIn("stage=head-assertion", err)
        self.assertEqual(self.transport_used.requests, [])
        self.assertEqual(self.processes.dir_calls(), [])

    def test_enumerates_only_tracked_regular_blobs(self) -> None:
        self.repository.add("link", b"target", mode="120000")
        self.repository.add("vendor/module", b"", mode="160000")
        blobs = self.module.enumerate_tracked_blobs(self.repository_path, self.processes)
        self.assertEqual([blob.path for blob in blobs], self.repository.regular_paths())
        self.assertEqual({blob.mode for blob in blobs}, {"100644", "100755"})
        for blob in blobs:
            self.assertEqual(blob.oid, blob_oid(self.repository.files[blob.path][1]))

    def test_rejects_malformed_enumeration_records(self) -> None:
        oid = blob_oid(b"x")
        cases = {
            "garbage": [b"garbage"],
            "short oid": [record("100644", "blob", "abc", b"x")],
            "duplicate path": [record("100644", "blob", oid, b"README.md")],
            "invalid utf-8": [record("100644", "blob", oid, b"\xff.txt")],
            "absolute": [record("100644", "blob", oid, b"/etc/passwd")],
            "drive": [record("100644", "blob", oid, b"C:/x")],
            "unc": [record("100644", "blob", oid, b"//server/share/x")],
            "backslash": [record("100644", "blob", oid, b"a\\b")],
            "empty component": [record("100644", "blob", oid, b"a//b")],
            "dot component": [record("100644", "blob", oid, b"./x")],
            "dotdot component": [record("100644", "blob", oid, b"a/../x")],
            "trailing slash": [record("100644", "blob", oid, b"a/")],
            "empty path": [record("100644", "blob", oid, b"")],
            "unexpected mode": [record("100664", "blob", oid, b"x")],
            "tree record": [record("040000", "tree", oid, b"dir")],
            "type mismatch": [record("100644", "commit", oid, b"x")],
            "uppercase oid": [record("100644", "blob", oid.upper(), b"x")],
        }
        for label, records in cases.items():
            with self.subTest(case=label):
                self.repository.extra_records = records
                with self.assertRaises(self.error):
                    self.module.enumerate_tracked_blobs(self.repository_path, self.processes)
        self.repository.extra_records = []
        self.repository.files.clear()
        with self.assertRaises(self.error):
            self.module.enumerate_tracked_blobs(self.repository_path, self.processes)

    def materialize(self) -> Path:
        scan_root = self.root / "scan-tree"
        scan_root.mkdir(exist_ok=True)
        blobs = self.module.enumerate_tracked_blobs(self.repository_path, self.processes)
        self.module.materialize_blobs(self.repository_path, blobs, scan_root, self.processes)
        return scan_root

    def test_materializes_exact_blob_content(self) -> None:
        self.repository.add("link", b"target", mode="120000")
        scan_root = self.materialize()
        written = sorted(p.relative_to(scan_root).as_posix() for p in scan_root.rglob("*") if p.is_file())
        self.assertEqual(written, self.repository.regular_paths())
        for path in written:
            self.assertEqual((scan_root / path).read_bytes(), self.repository.files[path][1])

    def test_rejects_materialization_faults(self) -> None:
        oid = blob_oid(b"int value;\n")
        self.repository.blob_override[oid] = b"int other;\n"
        with self.assertRaisesRegex(self.error, "object"):
            self.materialize()
        self.repository.blob_override.clear()
        self.repository.cat_file_failures.add(oid)
        with self.assertRaisesRegex(self.error, "cat-file"):
            self.materialize()
        self.repository.cat_file_failures.clear()
        collision = self.root / "scan-tree" / "README.md"
        collision.parent.mkdir(parents=True, exist_ok=True)
        collision.write_bytes(b"pre-existing")
        with self.assertRaisesRegex(self.error, "exists"):
            self.materialize()
        collision.unlink()
        try:
            os.symlink(self.root / "elsewhere", self.root / "scan-tree" / "src")
        except (OSError, NotImplementedError):
            self.skipTest("symbolic links are unavailable")
        with self.assertRaisesRegex(self.error, "symbolic"):
            self.materialize()

    def test_validates_exact_configuration(self) -> None:
        config = self.root / ".gitleaks.toml"
        config.write_text(VALID_CONFIG, encoding="utf-8")
        self.module.validate_configuration(config)
        cases = {
            "no extend": VALID_CONFIG.replace("[extend]\nuseDefault = true\n", ""),
            "default disabled": VALID_CONFIG.replace("useDefault = true", "useDefault = false"),
            "extra extend key": VALID_CONFIG.replace("useDefault = true", 'useDefault = true\npath = "x"'),
            "allowlist": VALID_CONFIG + '\n[allowlist]\npaths = [".*"]\n',
            "rule allowlist": VALID_CONFIG + '\n[rules.allowlist]\nregexes = ["x"]\n',
            "extra rule": VALID_CONFIG + '\n[[rules]]\nid = "other"\nregex = "x"\n',
            "no rule": VALID_CONFIG.split("[[rules]]")[0],
            "other id": VALID_CONFIG.replace("sitometron-synthetic-canary", "other"),
            "other keyword": VALID_CONFIG.replace('["SITOMETRON_CANARY_"]', '["OTHER_"]'),
            "extra keyword": VALID_CONFIG.replace('["SITOMETRON_CANARY_"]', '["SITOMETRON_CANARY_", "x"]'),
            "other regex": VALID_CONFIG.replace("[0-9a-f]{32}", "[0-9a-f]{8}"),
            "entropy": VALID_CONFIG + "entropy = 3.0\n",
            "path filter": VALID_CONFIG + "path = '''x'''\n",
            "tags": VALID_CONFIG + 'tags = ["x"]\n',
            "extra top-level key": VALID_CONFIG + '\n[foo]\nbar = 1\n',
            "empty title": VALID_CONFIG.replace('"Sitometron secret scan"', '""'),
            "no title": VALID_CONFIG.replace('title = "Sitometron secret scan"\n', ""),
            "malformed toml": VALID_CONFIG + "\n[[rules\n",
            "invalid utf-8": None,
        }
        for label, text in cases.items():
            with self.subTest(case=label):
                if text is None:
                    config.write_bytes(b"title = \"\xff\"\n")
                else:
                    config.write_text(text, encoding="utf-8")
                with self.assertRaises(self.error):
                    self.module.validate_configuration(config)

    def test_clean_repository_runs_every_probe_then_scans_exact_head(self) -> None:
        self.repository.add("link", b"target", mode="120000")
        code, out, err = self.run_scan()
        self.assertEqual(code, self.module.EXIT_CLEAN, err)
        self.assertIn("category=clean", out)
        self.assertEqual(self.transport_used.requests, [FIXTURES.INITIAL_URL, FIXTURES.SIGNED_URL])
        calls = self.processes.dir_calls()
        self.assertEqual([Path(args[2]).name for args in calls], [*PROBE_NAMES, "scan-tree"])
        self.assertEqual(self.processes.scan_snapshots["scan-tree"], self.repository.regular_paths())
        self.assertEqual(self.processes.ignore_sizes, [0] * 5)
        for args in calls:
            flags = args[3:]
            for flag in ["--no-banner", "--no-color", "--redact=100", "--ignore-gitleaks-allow"]:
                self.assertIn(flag, flags)
            self.assertEqual(flags[flags.index("--exit-code") + 1], "1")
            self.assertTrue(flags[flags.index("--config") + 1].endswith("scan-tree/.gitleaks.toml") or
                            flags[flags.index("--config") + 1].endswith("scan-tree\\.gitleaks.toml"))
            self.assertTrue(Path(flags[flags.index("--gitleaks-ignore-path") + 1]).name.endswith("gitleaksignore"))
            for forbidden in ["--report-path", "-r", "-v", "--verbose", "--log-level"]:
                self.assertNotIn(forbidden, flags)
        for args, kwargs in self.processes.calls:
            if args[1] == "dir":
                self.assertEqual(Path(str(kwargs["cwd"])).parent, self.workspaces)
                self.assertTrue(kwargs.get("capture_output"))

    def test_classifies_failures_and_stops_later_stages(self) -> None:
        cases = {
            "version pin": ("pin", "policy-failure", 0, 0),
            "configuration": ("config", "policy-failure", 0, 0),
            "checksum": ("checksum", "acquisition-failure", 2, 0),
            "version": ("version", "acquisition-failure", 2, 0),
            "positive probe silent": ({"probe-positive": 0}, "probe-failure", 2, 1),
            "near-match flagged": ({"probe-near-match": 1}, "probe-failure", 2, 2),
            "inline allow honored": ({"probe-inline-allow": 0}, "probe-failure", 2, 3),
            "ignore state honored": ({"probe-ignore-state": 0}, "probe-failure", 2, 4),
            "probe tool error": ({"probe-positive": 3}, "tool-failure", 2, 1),
            "repository finding": ({"scan-tree": 1}, "finding", 2, 5),
            "repository tool error": ({"scan-tree": 126}, "tool-failure", 2, 5),
            "process start": ("start", "tool-failure", 2, 5),
        }
        for label, (setup, category, requests, dir_calls) in cases.items():
            with self.subTest(case=label):
                self.reset()
                if setup == "pin":
                    self.repository.add("tools/gitleaks-tool-version.txt", b"8.30.1")
                elif setup == "config":
                    self.repository.add(".gitleaks.toml", b'title = "x"\n')
                elif setup == "checksum":
                    self.repository.add(
                        "tools/gitleaks-linux-x64.sha256", f"{'0' * 64}  {FIXTURES.ARCHIVE_NAME}\n".encode()
                    )
                elif setup == "version":
                    self.processes.version = b"8.30.2\n"
                elif setup == "start":
                    self.processes.start_failures.add("scan-tree")
                else:
                    self.processes.exit_overrides.update(setup)
                code, out, err = self.run_scan()
                expected = self.module.EXIT_FINDING if category == "finding" else self.module.EXIT_FAILURE
                self.assertEqual(code, expected, err)
                self.assertIn(f"category={category}", out + err)
                self.assertEqual(len(self.transport_used.requests), requests)
                self.assertEqual(len(self.processes.dir_calls()), dir_calls)


if __name__ == "__main__":
    unittest.main()
