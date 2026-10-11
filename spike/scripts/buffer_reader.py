#!/usr/bin/env python3
"""Read one durable Session buffer through raw Zenoh."""
from __future__ import annotations

import argparse
import sys
import time

import zenoh


class BufferReadError(RuntimeError):
    """Indicate that the raw Zenoh query could not be completed reliably."""


CONTROL_KEY = "base/input"


def _config(config_json: str | None):
    return zenoh.Config.from_json5(config_json) if config_json else zenoh.Config()


def _reply_payload(reply, key: str) -> bytes | None:
    if reply.err is not None:
        raise BufferReadError(f"Zenoh query returned an error for {key}: {reply.err}")
    if reply.ok is None:
        raise BufferReadError(f"Zenoh query returned no sample or error for {key}")
    sample = reply.ok
    if str(sample.key_expr) != key:
        return None
    return sample.payload.to_bytes()


def _samples(session, key: str) -> list[bytes]:
    try:  # noqa: BLE001 - normalize third-party query failures for the CLI and worker.
        replies = list(session.get(key, timeout=0.5))
        samples = []
        for reply in replies:
            payload = _reply_payload(reply, key)
            if payload is not None:
                samples.append(payload)
        return samples
    except BufferReadError:
        raise
    except Exception as error:  # noqa: BLE001 - the binding may expose different transport errors.
        raise BufferReadError(f"Zenoh query failed for {key}: {type(error).__name__}: {error}") from error


def _matches(samples: list[bytes], expect_missing: bool) -> bool:
    return not samples if expect_missing else samples == [b"durable-value"]


def _require_control_reply(session, prefix: str, deadline: float) -> None:
    key = f"{prefix}/{CONTROL_KEY}"
    last_error: BufferReadError | None = None
    while time.monotonic() < deadline:
        try:
            samples = _samples(session, key)
        except BufferReadError as error:
            last_error = error
        else:
            if samples:
                return
            last_error = None
        time.sleep(0.05)
    if last_error is not None:
        raise BufferReadError(
            f"control query failed for {key}; cannot prove the node is alive: {last_error}"
        ) from last_error
    raise BufferReadError(
        f"control query received no reply for {key}; cannot prove durable absence"
    )


def _report_success(key: str, expect_missing: bool) -> None:
    if expect_missing:
        print(f"SITOS_READER_MISSING {key}", flush=True)
    else:
        print(f"SITOS_READER_OK {key} durable-value", flush=True)


def read_durable_buffer(
    prefix: str,
    session_id: str,
    buffer_key: str,
    zenoh_config: str | None,
    expect_missing: bool = False,
) -> bool:
    """Wait for the expected durable value, or its absence after Session close."""
    key = f"{prefix}/buffers/{session_id}/durable/{buffer_key}"
    try:  # noqa: BLE001 - normalize third-party session failures for the CLI and worker.
        with zenoh.open(_config(zenoh_config)) as session:
            deadline = time.monotonic() + 4.0
            if expect_missing:
                _require_control_reply(session, prefix, deadline)
                deadline = time.monotonic() + 4.0
            while time.monotonic() < deadline:
                if _matches(_samples(session, key), expect_missing):
                    if expect_missing:
                        # An empty reply proves absence only if the node still answers.
                        _require_control_reply(session, prefix, time.monotonic() + 4.0)
                    _report_success(key, expect_missing)
                    return True
                time.sleep(0.05)
    except BufferReadError:
        raise
    except Exception as error:  # noqa: BLE001 - the binding may expose different session errors.
        raise BufferReadError(f"Zenoh session failed for {key}: {type(error).__name__}: {error}") from error

    if expect_missing:
        print(f"expected durable buffer to be absent: {key}", flush=True)
    else:
        print(f"durable buffer was not readable: {key}", flush=True)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--sid", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--zenoh-config")
    parser.add_argument("--expect-missing", action="store_true")
    args = parser.parse_args()
    try:
        succeeded = read_durable_buffer(
            args.prefix,
            args.sid,
            args.key,
            args.zenoh_config,
            args.expect_missing,
        )
    except BufferReadError as error:
        print(f"SITOS_READER_ERROR {error}", file=sys.stderr, flush=True)
        return 1
    return int(not succeeded)


if __name__ == "__main__":
    raise SystemExit(main())
