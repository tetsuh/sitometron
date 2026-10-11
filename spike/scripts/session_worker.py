#!/usr/bin/env python3
"""Run the Issue #98 Sitos Session worker scenario."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

import sitos

DURABLE_KEY = "worker/durable"
EPHEMERAL_KEY = "worker/ephemeral"
PREFIX_FALLBACK = "sitometron-spike"
SESSION_FALLBACK = "missing-session"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reader", required=True)
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--sleep-after", type=float, default=0.0)
    args = parser.parse_args()

    prefix = os.environ.get("SITOS_PREFIX", PREFIX_FALLBACK)
    session_id = os.environ.get("SITOS_SESSION_ID", SESSION_FALLBACK)
    try:
        with sitos.ParamCache(prefix=prefix, query_timeout_ms=2000) as cache:
            cache.attach(session_id)
            input_value = cache.get("input", type=str)

        output_value = f"worker:{input_value}"
        with sitos.ParamStore(prefix=prefix, query_timeout_ms=2000) as store:
            scope = f"session/{session_id}"
            store.put(scope, "output", output_value, ack=True, ack_timeout_ms=2000)
            if store.get(scope, "output", type=str) != output_value:
                raise RuntimeError("Session output parameter did not round-trip")

        durable = sitos.BufferPublisher(
            session_id,
            sitos.BufferClass.DURABLE,
            prefix=prefix,
            query_timeout_ms=2000,
        )
        ephemeral = sitos.BufferPublisher(
            session_id,
            sitos.BufferClass.EPHEMERAL,
            prefix=prefix,
            query_timeout_ms=2000,
        )
        durable.push(DURABLE_KEY, b"durable-value")
        ephemeral.push(EPHEMERAL_KEY, b"ephemeral-value")
        durable.fence(sitos.FenceDurability.APPLIED, timeout=2.0)
        ephemeral.fence(sitos.FenceDurability.APPLIED, timeout=2.0)

        subprocess.run(
            [
                sys.executable,
                args.reader,
                "--prefix",
                prefix,
                "--sid",
                session_id,
                "--key",
                DURABLE_KEY,
            ],
            check=True,
        )
        print(
            f"SITOS_WORKER_PARAMETER_OK input={input_value} output={output_value}",
            flush=True,
        )
        if args.sleep_after > 0:
            time.sleep(args.sleep_after)
        return args.exit_code
    except BaseException as error:  # the process status is part of the scenario
        print(
            f"SITOS_WORKER_ERROR {type(error).__name__}: {error}",
            file=sys.stderr,
            flush=True,
        )
        return args.exit_code if args.exit_code != 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
