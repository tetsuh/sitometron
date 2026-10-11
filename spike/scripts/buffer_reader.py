#!/usr/bin/env python3
"""Read one durable Session buffer through raw Zenoh."""
from __future__ import annotations

import argparse
import time

import zenoh


def _config(config_json: str | None):
    return zenoh.Config.from_json5(config_json) if config_json else zenoh.Config()


def _reply_payload(reply, key: str) -> bytes | None:
    if reply.err is not None or reply.ok is None:
        return None
    sample = reply.ok
    if str(sample.key_expr) != key:
        return None
    return sample.payload.to_bytes()


def _samples(session, key: str) -> list[bytes]:
    try:
        replies = list(session.get(key, timeout=0.5))
    except Exception:
        return []
    samples = []
    for reply in replies:
        payload = _reply_payload(reply, key)
        if payload is not None:
            samples.append(payload)
    return samples


def _matches(samples: list[bytes], expect_missing: bool) -> bool:
    return not samples if expect_missing else samples == [b"durable-value"]


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
    deadline = time.monotonic() + 4.0
    with zenoh.open(_config(zenoh_config)) as session:
        while time.monotonic() < deadline:
            if _matches(_samples(session, key), expect_missing):
                _report_success(key, expect_missing)
                return True
            time.sleep(0.05)

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
    return int(
        not read_durable_buffer(
            args.prefix,
            args.sid,
            args.key,
            args.zenoh_config,
            args.expect_missing,
        )
    )


if __name__ == "__main__":
    raise SystemExit(main())
