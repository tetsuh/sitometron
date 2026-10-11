#!/usr/bin/env python3
"""Read one durable Session buffer through raw Zenoh."""
from __future__ import annotations

import argparse
import time

import zenoh


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--sid", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--zenoh-config")
    parser.add_argument("--expect-missing", action="store_true")
    args = parser.parse_args()

    key = f"{args.prefix}/buffers/{args.sid}/durable/{args.key}"
    deadline = time.monotonic() + 4.0
    config_json = args.zenoh_config
    config = zenoh.Config.from_json5(config_json) if config_json else zenoh.Config()
    with zenoh.open(config) as session:
        while time.monotonic() < deadline:
            try:
                replies = list(session.get(key, timeout=0.5))
            except Exception:
                replies = []
            samples = []
            for reply in replies:
                if reply.err is not None or reply.ok is None:
                    continue
                sample = reply.ok
                if str(sample.key_expr) == key:
                    samples.append(sample.payload.to_bytes())
            if args.expect_missing:
                if not samples:
                    print(f"SITOS_READER_MISSING {key}", flush=True)
                    return 0
            elif len(samples) == 1 and samples[0] == b"durable-value":
                print(
                    f"SITOS_READER_OK {key} durable-value",
                    flush=True,
                )
                return 0
            time.sleep(0.05)

    if args.expect_missing:
        print(f"expected durable buffer to be absent: {key}", flush=True)
    else:
        print(f"durable buffer was not readable: {key}", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
