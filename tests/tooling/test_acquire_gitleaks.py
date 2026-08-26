import gzip
import hashlib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from email.message import Message
from unittest.mock import patch
from pathlib import Path
from types import ModuleType
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HELPER = REPOSITORY_ROOT / "tools" / "acquire_gitleaks.py"
VERSION = "8.30.1"
ARCHIVE_NAME = f"gitleaks_{VERSION}_linux_x64.tar.gz"
INITIAL_URL = (
    "https://github.com/gitleaks/gitleaks/releases/download/"
    f"v{VERSION}/{ARCHIVE_NAME}"
)
SIGNED_URL = (
    "https://release-assets.githubusercontent.com/github-production-release-asset/1/2"
    "?sp=r&sig=SIGNED-QUERY-SENTINEL-0123456789"
)
MIB = 1024 * 1024

def load_helper() -> ModuleType:
    spec = importlib.util.spec_from_file_location("acquire_gitleaks", HELPER)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load acquisition helper")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module

def tar_header(
    name: bytes,
    size: int,
    *,
    typeflag: bytes = b"0",
    mode: int = 0o644,
    linkname: bytes = b"",
    prefix: bytes = b"",
    size_field: bytes | None = None,
    checksum_field: bytes | None = None,
) -> bytes:
    block = bytearray(512)
    block[0 : len(name)] = name
    block[100:108] = f"{mode:07o}\0".encode("ascii")
    block[108:116] = b"0000000\0"
    block[116:124] = b"0000000\0"
    block[124:136] = size_field or f"{size:011o}\0".encode("ascii")
    block[136:148] = b"00000000000\0"
    block[148:156] = b" " * 8
    block[156:157] = typeflag
    block[157 : 157 + len(linkname)] = linkname
    block[257:265] = b"ustar\x0000"
    block[345 : 345 + len(prefix)] = prefix
    block[148:156] = checksum_field or f"{sum(block):06o}\0 ".encode("ascii")
    return bytes(block)

def tar_member(name: bytes, data: bytes, **header_options: object) -> bytes:
    header = tar_header(name, len(data), **header_options)  # type: ignore[arg-type]
    return header + data + b"\0" * ((-len(data)) % 512)

def build_archive(
    members: list[bytes], *, terminate: bool = True, terminator_blocks: int = 2,
    trailing: bytes = b"",
) -> bytes:
    payload = b"".join(members)
    if terminate:
        payload += b"\0" * (512 * terminator_blocks)
    return gzip.compress(payload + trailing)

GITLEAKS_BODY = b"#!/bin/sh\nprintf '%s\\n' 8.30.1\n"
def valid_members() -> dict[str, bytes]:
    return {
        "LICENSE": tar_member(b"LICENSE", b"MIT\n"),
        "README.md": tar_member(b"README.md", b"# gitleaks\n"),
        "gitleaks": tar_member(b"gitleaks", GITLEAKS_BODY, mode=0o755),
    }


def valid_archive() -> bytes:
    return build_archive(list(valid_members().values()))

class FakeResponse:
    def __init__(self, status: int, headers: list[tuple[str, str]], body: bytes = b"") -> None:
        self.status = status
        self.headers = Message()
        for name, value in headers:
            self.headers.add_header(name, value)
        self._body = io.BytesIO(body)
        self.closed = False

    def read(self, amount: int = -1) -> bytes:
        return self._body.read(amount)

    def close(self) -> None:
        self.closed = True

def ok_response(body: bytes, length: str | None = None) -> FakeResponse:
    headers = [("Content-Type", "application/octet-stream")]
    if length != "":
        headers.append(("Content-Length", length if length is not None else str(len(body))))
    return FakeResponse(200, headers, body)

def redirect(location: str | None, status: int = 302) -> FakeResponse:
    return FakeResponse(status, [] if location is None else [("Location", location)])

class FakeTransport:
    def __init__(self, responses: dict[str, FakeResponse]) -> None:
        self.responses = responses
        self.requests: list[str] = []

    def __call__(self, url: str) -> FakeResponse:
        self.requests.append(url)
        if url not in self.responses:
            raise AssertionError(f"unexpected request: {url}")
        response = self.responses[url]
        if isinstance(response, BaseException):
            raise response
        return response

def completed(args: list[str], returncode: int = 0, stdout: bytes = b"") -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess(args, returncode, stdout=stdout, stderr=b"")


class FakeProcessRunner:
    def __init__(self, stdout: bytes = b"8.30.1\n", returncode: int = 0, start_error: bool = False) -> None:
        self.stdout = stdout
        self.returncode = returncode
        self.start_error = start_error
        self.calls: list[list[str]] = []

    def __call__(self, args: list[str], **_: object) -> subprocess.CompletedProcess:
        self.calls.append(list(args))
        if self.start_error:
            raise OSError("cannot start")
        return completed(args, self.returncode, self.stdout)


def write_pins(tools: Path, version: bytes = b"8.30.1\n", checksum: bytes | None = None) -> None:
    tools.mkdir(parents=True, exist_ok=True)
    if checksum is None:
        digest = hashlib.sha256(valid_archive()).hexdigest()
        checksum = f"{digest}  {ARCHIVE_NAME}\n".encode("ascii")
    (tools / "gitleaks-tool-version.txt").write_bytes(version)
    (tools / "gitleaks-linux-x64.sha256").write_bytes(checksum)


class AcquireGitleaksPresenceTest(unittest.TestCase):
    def test_helper_exists(self) -> None:
        self.assertTrue(HELPER.is_file(), "tools/acquire_gitleaks.py is absent")

@unittest.skipUnless(HELPER.is_file(), "acquisition helper is not implemented")
class AcquireGitleaksTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name).resolve()
        self.module = load_helper()
        self.error = self.module.AcquisitionError

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_parses_exact_pins_and_constructs_frozen_url(self) -> None:
        write_pins(self.root / "tools")
        pin = self.module.read_tool_pin(self.root / "tools")
        self.assertEqual(pin.version, VERSION)
        self.assertEqual(pin.archive_name, ARCHIVE_NAME)
        self.assertEqual(pin.sha256, hashlib.sha256(valid_archive()).hexdigest())
        self.assertEqual(pin.url, INITIAL_URL)

    def test_rejects_malformed_pin_bytes(self) -> None:
        digest = "a" * 64
        malformed_versions = [b"8.30.1", b"8.30.1\r\n", b" 8.30.1\n", b"8.30.1\n\n", b"v8.30.1\n", b"", b"8.30\n", b"8.30.1\n8.30.1\n", b"\xef\xbb\xbf8.30.1\n", b"8.30.1 \n"]
        malformed_checksums = [f"{digest.upper()}  {ARCHIVE_NAME}\n", f"{digest} {ARCHIVE_NAME}\n", f"{digest}  other.tar.gz\n", f"{digest}  {ARCHIVE_NAME}", f"{digest}  {ARCHIVE_NAME}\r\n", f"{digest}  {ARCHIVE_NAME}\n\n", f"{digest[:63]}  {ARCHIVE_NAME}\n", f"{digest}   {ARCHIVE_NAME}\n", f"{digest}  {ARCHIVE_NAME} \n", ""]
        for version in malformed_versions:
            with self.subTest(version=version):
                write_pins(self.root / "tools", version=version)
                with self.assertRaises(self.error):
                    self.module.read_tool_pin(self.root / "tools")
        for checksum in malformed_checksums:
            with self.subTest(checksum=checksum):
                write_pins(self.root / "tools", checksum=checksum.encode("ascii"))
                with self.assertRaises(self.error):
                    self.module.read_tool_pin(self.root / "tools")

    def test_rejects_missing_pin_files(self) -> None:
        with self.assertRaises(self.error):
            self.module.read_tool_pin(self.root / "absent")

    def test_validates_request_targets(self) -> None:
        self.assertEqual(self.module.validate_target(INITIAL_URL), INITIAL_URL)
        self.assertEqual(self.module.validate_target(SIGNED_URL), SIGNED_URL)
        self.assertEqual(
            self.module.validate_target("https://github.com:443/a"), "https://github.com:443/a"
        )
        forbidden = [
            "http://github.com/a", "ftp://github.com/a", "https://objects.githubusercontent.com/a",
            "https://evil.example/a", "https://user@github.com/a", "https://user:pw@github.com/a",
            "https://github.com:8443/a", "https://github.com/a?x=1", "https://github.com/a?",
            "https://github.com/a#frag", f"{SIGNED_URL}#frag", "/relative/path", "github.com/a",
            "https://", "https:///a", "https://github.com:abc/a", "https://github.com",
            "https://release-assets.githubusercontent.com/a" + "x" * 8192,
            "https://github.com/a\n", "https://GITHUB.com.evil.example/a",
        ]
        for url in forbidden:
            with self.subTest(url=url):
                with self.assertRaises(self.error):
                    self.module.validate_target(url)

    def download(self, responses: dict[str, FakeResponse]) -> tuple[FakeTransport, Path]:
        transport = FakeTransport(responses)
        destination = self.root / "archive.tar.gz"
        self.module.download(INITIAL_URL, destination, fetch=transport)
        return transport, destination

    def test_downloads_after_zero_one_or_two_redirects(self) -> None:
        body = valid_archive()
        hop = "https://github.com/gitleaks/gitleaks/releases/download/hop"
        sequences = [
            {INITIAL_URL: ok_response(body)},
            {INITIAL_URL: redirect(SIGNED_URL), SIGNED_URL: ok_response(body)},
            {INITIAL_URL: redirect(SIGNED_URL, 303), SIGNED_URL: ok_response(body)},
            {INITIAL_URL: redirect(SIGNED_URL, 308), SIGNED_URL: ok_response(body)},
            {INITIAL_URL: redirect(hop, 301), hop: redirect(SIGNED_URL, 307), SIGNED_URL: ok_response(body)},
        ]
        for responses in sequences:
            with self.subTest(hops=len(responses) - 1):
                transport, destination = self.download(responses)
                self.assertEqual(transport.requests, list(responses))
                self.assertEqual(destination.read_bytes(), body)
                self.assertTrue(all(response.closed for response in responses.values()))
                destination.unlink()

    def test_rejects_redirect_and_response_policy_violations(self) -> None:
        body = valid_archive()
        hop = "https://github.com/hop"
        hop2 = "https://github.com/hop2"
        cases = {
            "three redirects": {
                INITIAL_URL: redirect(hop), hop: redirect(hop2), hop2: redirect(SIGNED_URL),
                SIGNED_URL: ok_response(body),
            },
            "missing location": {INITIAL_URL: redirect(None)},
            "duplicate location": {
                INITIAL_URL: FakeResponse(302, [("Location", SIGNED_URL), ("Location", hop)])
            },
            "relative location": {INITIAL_URL: redirect("/relative")},
            "fragment location": {INITIAL_URL: redirect(f"{SIGNED_URL}#frag")},
            "oversized location": {INITIAL_URL: redirect(SIGNED_URL + "x" * 8192)},
            "malformed location": {INITIAL_URL: redirect("https://")},
            "http location": {INITIAL_URL: redirect("http://github.com/hop")},
            "foreign host": {INITIAL_URL: redirect("https://evil.example/hop")},
            "userinfo": {INITIAL_URL: redirect("https://user@github.com/hop")},
            "port": {INITIAL_URL: redirect("https://github.com:8443/hop")},
            "github query": {INITIAL_URL: redirect("https://github.com/hop?signed=1")},
            "status 300": {INITIAL_URL: FakeResponse(300, [("Location", SIGNED_URL)])},
            "status 304": {INITIAL_URL: FakeResponse(304, [])},
            "status 404": {INITIAL_URL: FakeResponse(404, [])},
            "status 500": {INITIAL_URL: FakeResponse(500, [])},
            "status 204": {INITIAL_URL: FakeResponse(204, [])},
            "missing length": {INITIAL_URL: ok_response(body, "")},
            "duplicate length": {
                INITIAL_URL: FakeResponse(
                    200, [("Content-Length", str(len(body))), ("Content-Length", "1")], body
                )
            },
            "malformed length": {INITIAL_URL: ok_response(body, "abc")},
            "signed length": {INITIAL_URL: ok_response(body, "+5")},
            "padded length": {INITIAL_URL: ok_response(body, " 5")},
            "leading zero length": {INITIAL_URL: ok_response(body, "05")},
            "zero length": {INITIAL_URL: ok_response(b"", "0")},
            "oversized length": {INITIAL_URL: ok_response(body, str(16 * MIB + 1))},
            "short body": {INITIAL_URL: ok_response(body, str(len(body) + 1))},
            "overlong body": {INITIAL_URL: ok_response(body, str(len(body) - 1))},
        }
        for label, responses in cases.items():
            with self.subTest(case=label):
                with self.assertRaises(self.error):
                    self.download(responses)
                self.assertFalse(
                    (self.root / "archive.tar.gz").exists(),
                    "no archive may remain after a failed download",
                )

    def test_stream_http_failure_is_bounded_and_cleans_partial_file(self) -> None:
        module = self.module
        class Broken(FakeResponse):
            def read(self, amount: int = -1) -> bytes:
                raise module.http.client.IncompleteRead(b"x", 2)

        destination = self.root / "archive.tar.gz"
        response = Broken(200, [("Content-Length", "1")], b"x")
        with self.assertRaises(self.error):
            self.module.download(INITIAL_URL, destination, fetch=FakeTransport({INITIAL_URL: response}))
        self.assertFalse(destination.exists())

    def test_write_failures_are_acquisition_errors_and_leave_no_file(self) -> None:
        destination = self.root / "missing-directory" / "archive.tar.gz"
        transport = FakeTransport({INITIAL_URL: ok_response(valid_archive())})
        with self.assertRaises(self.error):
            self.module.download(INITIAL_URL, destination, fetch=transport)
        self.assertFalse(destination.exists())
        archive = self.root / ARCHIVE_NAME
        archive.write_bytes(valid_archive())
        with self.assertRaises(self.error):
            self.module.extract_gitleaks(archive, self.root / "absent-target-directory")

    def test_download_refuses_existing_destination(self) -> None:
        destination = self.root / "archive.tar.gz"
        destination.write_bytes(b"old")
        transport = FakeTransport({INITIAL_URL: ok_response(valid_archive())})
        with self.assertRaises(self.error):
            self.module.download(INITIAL_URL, destination, fetch=transport)
        self.assertEqual(destination.read_bytes(), b"old")

    def test_verifies_checksum(self) -> None:
        archive = self.root / ARCHIVE_NAME
        archive.write_bytes(valid_archive())
        self.module.verify_checksum(archive, hashlib.sha256(valid_archive()).hexdigest())
        with self.assertRaises(self.error):
            self.module.verify_checksum(archive, "0" * 64)

    def inspect(self, data: bytes) -> bytes:
        archive = self.root / ARCHIVE_NAME
        archive.write_bytes(data)
        return self.module.inspect_archive(archive)

    def test_accepts_exact_three_member_archive(self) -> None:
        self.assertEqual(self.inspect(valid_archive()), GITLEAKS_BODY)
        members = valid_members()
        reordered = build_archive([members["gitleaks"], members["README.md"], members["LICENSE"]])
        self.assertEqual(self.inspect(reordered), GITLEAKS_BODY)

    def test_accepts_additional_zero_padding_after_end_marker(self) -> None:
        self.assertEqual(
            self.inspect(build_archive(list(valid_members().values()), terminator_blocks=3)),
            GITLEAKS_BODY,
        )

    def test_rejects_unsafe_archives(self) -> None:
        base = valid_members()
        big = b"\0" * (12 * MIB)

        def with_member(name: str, member: bytes) -> bytes:
            members = dict(base)
            members[name] = member
            return build_archive(list(members.values()))

        cases = {
            "missing member": build_archive([base["LICENSE"], base["gitleaks"]]), "extra member": build_archive([*base.values(), tar_member(b"CHANGELOG", b"x")]), "duplicate member": build_archive([*base.values(), base["LICENSE"]]),
            "absolute name": with_member("gitleaks", tar_member(b"/gitleaks", GITLEAKS_BODY, mode=0o755)), "separator name": with_member("gitleaks", tar_member(b"bin/gitleaks", GITLEAKS_BODY, mode=0o755)), "backslash name": with_member("gitleaks", tar_member(b"bin\\gitleaks", GITLEAKS_BODY, mode=0o755)),
            "dot name": with_member("LICENSE", tar_member(b".", b"x")), "dotdot name": with_member("LICENSE", tar_member(b"..", b"x")), "prefix name": with_member("gitleaks", tar_member(b"gitleaks", GITLEAKS_BODY, mode=0o755, prefix=b"bin")),
            "directory": with_member("LICENSE", tar_member(b"LICENSE", b"", typeflag=b"5")), "symlink": with_member("gitleaks", tar_member(b"gitleaks", b"", typeflag=b"2", linkname=b"/bin/sh")), "hard link": with_member("gitleaks", tar_member(b"gitleaks", b"", typeflag=b"1", linkname=b"LICENSE")),
            "character device": with_member("LICENSE", tar_member(b"LICENSE", b"", typeflag=b"3")), "block device": with_member("LICENSE", tar_member(b"LICENSE", b"", typeflag=b"4")), "fifo": with_member("LICENSE", tar_member(b"LICENSE", b"", typeflag=b"6")), "contiguous": with_member("LICENSE", tar_member(b"LICENSE", b"x", typeflag=b"7")), "unknown type": with_member("LICENSE", tar_member(b"LICENSE", b"x", typeflag=b"s")),
            "pax extended": build_archive([tar_member(b"PaxHeader", b"1 x\n", typeflag=b"x"), *base.values()]), "pax global": build_archive([tar_member(b"pax_global", b"1 x\n", typeflag=b"g"), *base.values()]), "gnu long name": build_archive([tar_member(b"././@LongLink", b"gitleaks\0", typeflag=b"L"), *base.values()]),
            "sparse": with_member("gitleaks", tar_member(b"gitleaks", GITLEAKS_BODY, typeflag=b"S", mode=0o755)), "link target on regular": with_member("LICENSE", tar_member(b"LICENSE", b"x", linkname=b"other")), "non-executable gitleaks": with_member("gitleaks", tar_member(b"gitleaks", GITLEAKS_BODY, mode=0o644)),
            "base-256 size": with_member("LICENSE", tar_header(b"LICENSE", 0, size_field=b"\x80" + b"\0" * 11)), "non-octal size": with_member("LICENSE", tar_header(b"LICENSE", 0, size_field=b"zzzzzzzzzzz\0")), "oversized member": with_member("LICENSE", tar_header(b"LICENSE", 32 * MIB + 1)),
            "oversized total": build_archive([
                tar_member(b"LICENSE", big), tar_member(b"README.md", big),
                tar_member(b"gitleaks", big, mode=0o755),
            ]),
            "single zero block terminator": build_archive(list(base.values()), terminator_blocks=1),
            "member after one zero block": build_archive(
                list(base.values()), terminator_blocks=1, trailing=tar_member(b"EVIL", b"x")
            ),
            "member after complete end marker": build_archive(
                list(base.values()), trailing=tar_member(b"EVIL", b"x")
            ),
            "nonzero trailing data": build_archive(list(base.values()), trailing=b"EVIL"),
            "truncated content": gzip.compress(tar_header(b"LICENSE", 4096) + b"x" * 100),
            "truncated header": gzip.compress(b"".join(base.values()) + b"x" * 100),
            "checksum mismatch": with_member("LICENSE", tar_member(b"LICENSE", b"x", checksum_field=b"000000\0 ")),
            "not gzip": b"plain bytes, not an archive",
            "empty archive": build_archive([]),
        }
        for label, data in cases.items():
            with self.subTest(case=label):
                with self.assertRaises(self.error):
                    self.inspect(data)

    def test_extracts_only_gitleaks_with_exclusive_creation(self) -> None:
        archive = self.root / ARCHIVE_NAME
        archive.write_bytes(valid_archive())
        target = self.root / "tool"
        target.mkdir()
        application = self.module.extract_gitleaks(archive, target)
        self.assertEqual(application, target / "gitleaks")
        self.assertEqual(sorted(path.name for path in target.iterdir()), ["gitleaks"])
        self.assertEqual(application.read_bytes(), GITLEAKS_BODY)
        self.assertTrue(os.access(application, os.X_OK))
        if os.name == "posix":
            self.assertEqual(application.stat().st_mode & 0o777, 0o700)
        with self.assertRaises(self.error):
            self.module.extract_gitleaks(archive, target)
        archive.write_bytes(build_archive([valid_members()["LICENSE"]]))
        other = self.root / "other"
        other.mkdir()
        with self.assertRaises(self.error):
            self.module.extract_gitleaks(archive, other)
        self.assertEqual(list(other.iterdir()), [])

    def test_requires_exact_reported_version(self) -> None:
        application = self.root / "gitleaks"
        application.write_bytes(GITLEAKS_BODY)
        runner = FakeProcessRunner()
        self.module.require_version(application, VERSION, run_process=runner)
        self.assertEqual(runner.calls, [[str(application), "version"]])
        failures = [
            FakeProcessRunner(stdout=b"v8.30.1\n"), FakeProcessRunner(stdout=b"8.30.2\n"),
            FakeProcessRunner(stdout=b""), FakeProcessRunner(stdout=b"8.30.1\n8.30.1\n"),
            FakeProcessRunner(stdout=b" 8.30.1\n"), FakeProcessRunner(stdout=b"8.30.1 \n"),
            FakeProcessRunner(stdout=b"8.30.1\r\n"),
            FakeProcessRunner(returncode=1), FakeProcessRunner(start_error=True),
        ]
        for failing in failures:
            with self.subTest(stdout=failing.stdout, returncode=failing.returncode):
                with self.assertRaises(self.error):
                    self.module.require_version(application, VERSION, run_process=failing)

    def acquire(
        self, *, archive: bytes | None = None, checksum: str | None = None,
        runner: FakeProcessRunner | None = None, responses: dict[str, FakeResponse] | None = None,
    ) -> tuple[object, FakeTransport, FakeProcessRunner, Path]:
        archive = valid_archive() if archive is None else archive
        digest = checksum or hashlib.sha256(archive).hexdigest()
        write_pins(self.root / "tools", checksum=f"{digest}  {ARCHIVE_NAME}\n".encode("ascii"))
        pin = self.module.read_tool_pin(self.root / "tools")
        transport = FakeTransport(
            responses if responses is not None else {INITIAL_URL: ok_response(archive)}
        )
        runner = runner or FakeProcessRunner()
        directory = self.root / "run"
        directory.mkdir()
        result = self.module.acquire(pin, directory, fetch=transport, run_process=runner)
        return result, transport, runner, directory

    def test_cleanup_failure_preserves_bounded_acquisition_error(self) -> None:
        with patch.object(Path, "unlink", side_effect=PermissionError("denied")) as unlink:
            with self.assertRaisesRegex(self.error, r"^archive SHA-256"):
                self.acquire(checksum="0" * 64)
        self.assertEqual(unlink.call_count, 2)

    def test_extraction_cleanup_failure_is_bounded(self) -> None:
        archive = self.root / ARCHIVE_NAME
        archive.write_bytes(valid_archive())
        target = self.root / "extract"
        target.mkdir()
        with patch.object(self.module.os, "chmod", side_effect=OSError("denied")):
            with patch.object(Path, "unlink", side_effect=PermissionError("denied")):
                with self.assertRaises(self.error):
                    self.module.extract_gitleaks(archive, target)
        self.assertTrue((target / "gitleaks").exists())

    def test_successful_archive_cleanup_failure_is_bounded(self) -> None:
        with patch.object(Path, "unlink", side_effect=PermissionError("denied")):
            with self.assertRaises(self.error) as raised:
                self.acquire()
        self.assertIn("cannot remove run-owned file", str(raised.exception))
        self.assertEqual(sorted(path.name for path in (self.root / "run").iterdir()), ["gitleaks", ARCHIVE_NAME])

    def test_acquire_runs_every_stage_in_order(self) -> None:
        application, transport, runner, directory = self.acquire()
        self.assertEqual(application, directory / "gitleaks")
        self.assertEqual(transport.requests, [INITIAL_URL])
        self.assertEqual(runner.calls, [[str(application), "version"]])
        self.assertEqual(sorted(path.name for path in directory.iterdir()), ["gitleaks"])

    def test_acquire_stops_after_the_first_failure(self) -> None:
        cases = {
            "download": {"responses": {INITIAL_URL: FakeResponse(404, [])}},
            "checksum": {"checksum": "0" * 64},
            "archive": {"archive": build_archive([valid_members()["LICENSE"]])},
            "version": {"runner": FakeProcessRunner(stdout=b"8.30.2\n")},
        }
        for label, options in cases.items():
            with self.subTest(stage=label):
                runner = options.get("runner") or FakeProcessRunner()
                with self.assertRaises(self.error):
                    self.acquire(runner=runner, **{k: v for k, v in options.items() if k != "runner"})  # type: ignore[arg-type]
                directory = self.root / "run"
                self.assertEqual(
                    list(directory.iterdir()), [], f"{label} failure must leave no run-owned file"
                )
                if label == "version":
                    self.assertEqual(len(runner.calls), 1)
                else:
                    self.assertEqual(runner.calls, [], f"{label} failure must not reach version qualification")
                directory.rmdir()


if __name__ == "__main__":
    unittest.main()
