#!/usr/bin/env python3
"""Acquire the pinned Gitleaks release archive with fail-closed validation.

The version and SHA-256 pins are repository-owned files; this module parses and
byte-validates them, downloads the official release archive through a bounded
HTTPS redirect policy, verifies the checksum, inspects the raw tar headers,
extracts only the ``gitleaks`` executable, and requires the exact reported
version. No stage runs after a prerequisite stage fails.
"""
from __future__ import annotations

import gzip
import hashlib
import http.client
import os
import re
import ssl
import subprocess
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol
from urllib.parse import urlsplit

VERSION_PIN_FILE = "gitleaks-tool-version.txt"
CHECKSUM_PIN_FILE = "gitleaks-linux-x64.sha256"
ARCHIVE_NAME_TEMPLATE = "gitleaks_{version}_linux_x64.tar.gz"
RELEASE_URL_TEMPLATE = (
    "https://github.com/gitleaks/gitleaks/releases/download/v{version}/{archive}"
)
ALLOWED_HOSTS = frozenset({"github.com", "release-assets.githubusercontent.com"})
SIGNED_QUERY_HOST = "release-assets.githubusercontent.com"
REDIRECT_STATUSES = frozenset({301, 302, 303, 307, 308})
MAX_REDIRECTS = 2
MAX_LOCATION_BYTES = 8192
MAX_DOWNLOAD_BYTES = 16 * 1024 * 1024
MAX_MEMBER_BYTES = 32 * 1024 * 1024
EXPECTED_MEMBERS = frozenset({"LICENSE", "README.md", "gitleaks"})
EXECUTABLE_MEMBER = "gitleaks"
TAR_BLOCK = 512
VERSION_PATTERN = re.compile(rb"\A(\d+\.\d+\.\d+)\n\Z")
CHECKSUM_PATTERN = re.compile(rb"\A([0-9a-f]{64}) {2}([A-Za-z0-9._-]+)\n\Z")
DECIMAL_PATTERN = re.compile(r"\A(?:0|[1-9]\d*)\Z", re.ASCII)
REGULAR_TYPEFLAGS = frozenset({b"0", b"\0"})
TYPEFLAG_NAMES = {
    b"1": "hard link", b"2": "symbolic link", b"3": "character device",
    b"4": "block device", b"5": "directory", b"6": "FIFO", b"7": "contiguous file",
    b"x": "PAX extended header", b"g": "PAX global header", b"L": "GNU long name",
    b"K": "GNU long link", b"S": "GNU sparse file", b"X": "extended header",
}


class AcquisitionError(RuntimeError):
    """A fail-closed acquisition error; messages never contain URLs or payloads."""

class HttpResponse(Protocol):
    status: int
    headers: Any

    def read(self, amount: int = ..., /) -> bytes: ...

    def close(self) -> None: ...


Fetch = Callable[[str], HttpResponse]
ProcessRunner = Callable[..., subprocess.CompletedProcess[Any]]


@dataclass(frozen=True)
class ToolPin:
    version: str
    archive_name: str
    sha256: str
    url: str

def _read_pin_file(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise AcquisitionError(f"cannot read pin file {path.name}: {error.strerror}") from error


def parse_version_pin(data: bytes) -> str:
    match = VERSION_PATTERN.match(data)
    if match is None:
        raise AcquisitionError("version pin must be exactly MAJOR.MINOR.PATCH plus one LF")
    return match.group(1).decode("ascii")


def parse_checksum_pin(data: bytes, archive_name: str) -> str:
    match = CHECKSUM_PATTERN.match(data)
    if match is None:
        raise AcquisitionError(
            "checksum pin must be exactly lowercase SHA-256, two spaces, archive name, one LF"
        )
    if match.group(2).decode("ascii") != archive_name:
        raise AcquisitionError("checksum pin names a different archive than the version pin")
    return match.group(1).decode("ascii")


def read_tool_pin(tools_directory: Path) -> ToolPin:
    version = parse_version_pin(_read_pin_file(tools_directory / VERSION_PIN_FILE))
    archive_name = ARCHIVE_NAME_TEMPLATE.format(version=version)
    sha256 = parse_checksum_pin(_read_pin_file(tools_directory / CHECKSUM_PIN_FILE), archive_name)
    url = RELEASE_URL_TEMPLATE.format(version=version, archive=archive_name)
    return ToolPin(version=version, archive_name=archive_name, sha256=sha256, url=validate_target(url))


def validate_target(url: str) -> str:
    """Validate one request target against the frozen HTTPS policy."""
    if len(url.encode("utf-8", errors="replace")) > MAX_LOCATION_BYTES:
        raise AcquisitionError("request target exceeds the permitted length")
    if any(character.isspace() for character in url) or "#" in url:
        raise AcquisitionError("request target contains whitespace or a fragment")
    parts = urlsplit(url)
    if parts.scheme != "https":
        raise AcquisitionError("request target must use the https scheme")
    if "@" in parts.netloc:
        raise AcquisitionError("request target must not contain userinfo")
    try:
        port = parts.port
    except ValueError as error:
        raise AcquisitionError("request target has a malformed port") from error
    if port not in (None, 443):
        raise AcquisitionError("request target must use the default HTTPS port")
    if parts.hostname not in ALLOWED_HOSTS:
        raise AcquisitionError("request target host is not permitted")
    if not parts.path.startswith("/") or not parts.path:
        raise AcquisitionError("request target must be an absolute https URL with a path")
    if parts.hostname != SIGNED_QUERY_HOST and (parts.query or url.endswith("?")):
        raise AcquisitionError("request target must not carry a query on this host")
    return url


def open_https(url: str, timeout: float = 60.0) -> http.client.HTTPResponse:
    """Issue one GET without following redirects; callers enforce the policy."""
    parts = urlsplit(url)
    context = ssl.create_default_context()
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.check_hostname = True
    context.verify_mode = ssl.CERT_REQUIRED
    connection = http.client.HTTPSConnection(
        parts.hostname or "", parts.port or 443, timeout=timeout, context=context
    )
    target = parts.path + (f"?{parts.query}" if parts.query else "")
    connection.request(
        "GET", target, headers={"User-Agent": "sitometron-secret-scan", "Accept": "application/octet-stream"}
    )
    return connection.getresponse()


def _content_length(headers: Any) -> int:
    values = headers.get_all("Content-Length") or []
    if len(values) != 1:
        raise AcquisitionError("final response must declare exactly one Content-Length")
    value = values[0]
    if not isinstance(value, str) or DECIMAL_PATTERN.match(value) is None:
        raise AcquisitionError("Content-Length is not a valid decimal value")
    length = int(value)
    if not 1 <= length <= MAX_DOWNLOAD_BYTES:
        raise AcquisitionError("Content-Length is outside the permitted download size")
    return length


def _remove(path: Path, primary: BaseException | None = None) -> None:
    try:
        path.unlink(missing_ok=True)
    except OSError as error:
        if primary is None:
            raise AcquisitionError(f"cannot remove run-owned file: {error.strerror}") from error


def _stream_to_file(response: HttpResponse, destination: Path, length: int) -> None:
    received = 0
    try:
        handle = open(destination, "xb")
    except OSError as error:
        raise AcquisitionError(f"cannot create download destination: {error.strerror}") from error
    try:
        with handle:
            while True:
                chunk = response.read(min(65536, length - received + 1))
                if not chunk:
                    break
                received += len(chunk)
                if received > length:
                    raise AcquisitionError("response body is longer than its declared Content-Length")
                handle.write(chunk)
        if received != length:
            raise AcquisitionError("response body is shorter than its declared Content-Length")
    except (AcquisitionError, OSError, http.client.HTTPException) as error:
        primary = error if isinstance(error, AcquisitionError) else AcquisitionError(
            f"cannot read or write download destination: {type(error).__name__}"
        )
        _remove(destination, primary)
        raise primary from error


def _next_target(headers: Any) -> str:
    locations = headers.get_all("Location") or []
    if len(locations) != 1 or not isinstance(locations[0], str):
        raise AcquisitionError("redirect must carry exactly one Location header")
    return validate_target(locations[0])


def download(url: str, destination: Path, fetch: Fetch = open_https) -> None:
    """Download ``url`` to ``destination`` under the frozen redirect and size policy."""
    current = validate_target(url)
    redirects = 0
    while True:
        try:
            response = fetch(current)
        except (OSError, http.client.HTTPException) as error:
            raise AcquisitionError(f"download request failed: {type(error).__name__}") from error
        failure: BaseException | None = None
        try:
            if response.status in REDIRECT_STATUSES:
                redirects += 1
                if redirects > MAX_REDIRECTS:
                    raise AcquisitionError("download exceeded the permitted redirect count")
                current = _next_target(response.headers)
                continue
            if response.status != 200:
                raise AcquisitionError(f"download received HTTP status {response.status}")
            _stream_to_file(response, destination, _content_length(response.headers))
            return
        except AcquisitionError as error:
            failure = error
            raise
        finally:
            try:
                response.close()
            except (OSError, http.client.HTTPException) as error:
                if failure is None:
                    raise AcquisitionError("download response could not close") from error


def verify_checksum(path: Path, expected_sha256: str) -> None:
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(65536), b""):
                digest.update(chunk)
    except OSError as error:
        raise AcquisitionError(f"cannot read the downloaded archive: {error.strerror}") from error
    if digest.hexdigest() != expected_sha256:
        raise AcquisitionError("archive SHA-256 does not match the repository-owned pin")


def _read_exact(stream: Any, size: int, what: str) -> bytes:
    try:
        data = stream.read(size)
    except (OSError, EOFError) as error:
        raise AcquisitionError(f"archive is not a readable gzip stream: {what}") from error
    if len(data) != size:
        raise AcquisitionError(f"archive is truncated inside {what}")
    return data


def _octal_field(block: bytes, start: int, end: int, what: str) -> int:
    field = block[start:end]
    if field and field[0] & 0x80:
        raise AcquisitionError(f"archive header uses an extended numeric {what}")
    text = field.rstrip(b"\0 ").lstrip(b" ")
    if not text or any(byte not in b"01234567" for byte in text):
        raise AcquisitionError(f"archive header has a malformed {what}")
    return int(text, 8)


def _member_name(block: bytes) -> str:
    if block[345:500].rstrip(b"\0"):
        raise AcquisitionError("archive member uses a name prefix")
    raw = block[0:100].rstrip(b"\0")
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AcquisitionError("archive member name is not valid UTF-8") from error
    if not name or name in {".", ".."} or "/" in name or "\\" in name or "\0" in name:
        raise AcquisitionError("archive member name is empty, relative, absolute, or nested")
    if name not in EXPECTED_MEMBERS:
        raise AcquisitionError("archive contains an unexpected member")
    return name


def _validate_header(block: bytes) -> tuple[str, int, int]:
    """Return ``(name, size, mode)`` for one validated regular-file tar header."""
    checksum = sum(block[:148]) + sum(b" " * 8) + sum(block[156:])
    if _octal_field(block, 148, 156, "header checksum") != checksum:
        raise AcquisitionError("archive header checksum mismatch")
    typeflag = block[156:157]
    if typeflag not in REGULAR_TYPEFLAGS:
        kind = TYPEFLAG_NAMES.get(typeflag, "unsupported entry")
        raise AcquisitionError(f"archive member type is not a regular file: {kind}")
    if block[157:257].rstrip(b"\0"):
        raise AcquisitionError("archive regular member carries a link target")
    name = _member_name(block)
    size = _octal_field(block, 124, 136, "size")
    mode = _octal_field(block, 100, 108, "mode")
    return name, size, mode


def _consume_archive_end(stream: Any, zero_block: bytes) -> None:
    end_block = _read_exact(stream, TAR_BLOCK, "the complete archive end marker")
    if end_block != zero_block:
        raise AcquisitionError("archive has an incomplete end marker")
    try:
        while trailing := stream.read(65536):
            if any(trailing):
                raise AcquisitionError("archive contains nonzero data after its end marker")
    except (OSError, EOFError) as error:
        raise AcquisitionError("archive has unreadable trailing data") from error


def _read_member(stream: Any, block: bytes, seen: set[str], total: int) -> tuple[str, bytes, int]:
    name, size, mode = _validate_header(block)
    if name in seen:
        raise AcquisitionError("archive contains a duplicate member")
    total += size
    if size > MAX_MEMBER_BYTES or total > MAX_MEMBER_BYTES:
        raise AcquisitionError("archive member or total size exceeds the permitted bound")
    data = _read_exact(stream, size, "member content")
    _read_exact(stream, (-size) % TAR_BLOCK, "member padding")
    if name == EXECUTABLE_MEMBER and not mode & 0o100:
        raise AcquisitionError("archive gitleaks member is not owner-executable")
    return name, data, total


def inspect_archive(path: Path) -> bytes:
    """Validate every raw tar header and return the bytes of the ``gitleaks`` member."""
    seen: set[str] = set()
    total = 0
    executable: bytes | None = None
    zero_block = b"\0" * TAR_BLOCK
    try:
        stream = gzip.open(path, "rb")
    except OSError as error:
        raise AcquisitionError("archive cannot be opened") from error
    with stream:
        while True:
            block = _read_exact(stream, TAR_BLOCK, "a member header")
            if block == zero_block:
                _consume_archive_end(stream, zero_block)
                break
            name, data, total = _read_member(stream, block, seen, total)
            if name == EXECUTABLE_MEMBER:
                executable = data
            seen.add(name)
    if seen != EXPECTED_MEMBERS or executable is None:
        raise AcquisitionError("archive does not contain exactly the three expected members")
    return executable


def extract_gitleaks(archive: Path, directory: Path) -> Path:
    """Write only the validated ``gitleaks`` member into ``directory`` with exclusive creation."""
    data = inspect_archive(archive)
    target = directory / EXECUTABLE_MEMBER
    try:
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o700)
    except OSError as error:
        raise AcquisitionError(f"cannot create the extracted executable: {error.strerror}") from error
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
        os.chmod(target, 0o700)
    except OSError as error:
        target.unlink(missing_ok=True)
        raise AcquisitionError(f"cannot write the extracted executable: {error.strerror}") from error
    if not os.access(target, os.X_OK):
        raise AcquisitionError("extracted gitleaks is not executable")
    return target


def _text(value: Any) -> str:
    return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else str(value or "")


def require_version(application: Path, version: str, run_process: ProcessRunner = subprocess.run) -> None:
    try:
        result = run_process([str(application), "version"], capture_output=True, check=False)
    except OSError as error:
        raise AcquisitionError(f"gitleaks version command could not start: {error.strerror}") from error
    if result.returncode != 0:
        raise AcquisitionError(f"gitleaks version command failed with exit {result.returncode}")
    if result.stdout != (version + "\n").encode("ascii"):
        raise AcquisitionError("gitleaks reported a version that differs from the repository pin")


def acquire(
    pin: ToolPin,
    directory: Path,
    *,
    fetch: Fetch = open_https,
    run_process: ProcessRunner = subprocess.run,
) -> Path:
    """Download, verify, extract, and qualify the pinned tool inside ``directory``."""
    archive = directory / pin.archive_name
    application = directory / EXECUTABLE_MEMBER
    try:
        download(pin.url, archive, fetch)
        verify_checksum(archive, pin.sha256)
        extract_gitleaks(archive, directory)
        archive.unlink()
        require_version(application, pin.version, run_process)
    except AcquisitionError as error:
        for leftover in (archive, application):
            _remove(leftover, error)
        raise
    return application
