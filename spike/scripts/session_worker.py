#!/usr/bin/env python3
"""Run the Issue #98 Sitos Session worker scenario."""
from __future__ import annotations

import argparse
import os
import sys
import time

import sitos
import zenoh

from buffer_reader import read_durable_buffer

DURABLE_KEY = "worker/durable"
EPHEMERAL_KEY = "worker/ephemeral"
PREFIX_FALLBACK = "sitometron-spike"
SESSION_FALLBACK = "missing-session"
UNEXPECTED_ERROR_EXIT_CODE = 70


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--sleep-after", type=float, default=0.0)
    parser.add_argument("--publish-delay", type=float, default=0.0)
    args = parser.parse_args()

    prefix = os.environ.get("SITOS_PREFIX", PREFIX_FALLBACK)
    session_id = os.environ.get("SITOS_SESSION_ID", SESSION_FALLBACK)
    zenoh_config = os.environ.get("SITOS_ZENOH_CONFIG")
    try:
        with sitos.ParamCache(
            prefix=prefix,
            zenoh_config_json=zenoh_config,
            query_timeout_ms=5000,
        ) as cache:
            cache.attach(session_id)
            input_value = cache.get("input", type=str)

        output_value = f"worker:{input_value}"
        with sitos.ParamStore(
            prefix=prefix,
            zenoh_config_json=zenoh_config,
            query_timeout_ms=5000,
        ) as store:
            scope = f"session/{session_id}"
            store.put(scope, "output", output_value, ack=True, ack_timeout_ms=5000)
            if store.get(scope, "output", type=str) != output_value:
                raise RuntimeError("Session output parameter did not round-trip")

        if args.publish_delay > 0:
            time.sleep(args.publish_delay)

        publisher_error = None
        try:
            durable = sitos.BufferPublisher(
                session_id,
                sitos.BufferClass.DURABLE,
                prefix=prefix,
                zenoh_config_json=zenoh_config,
                query_timeout_ms=5000,
            )
            ephemeral = sitos.BufferPublisher(
                session_id,
                sitos.BufferClass.EPHEMERAL,
                prefix=prefix,
                zenoh_config_json=zenoh_config,
                query_timeout_ms=5000,
            )
            durable.push(DURABLE_KEY, b"durable-value")
            ephemeral.push(EPHEMERAL_KEY, b"ephemeral-value")
            durable.fence(sitos.FenceDurability.APPLIED, timeout=2.0)
            ephemeral.fence(sitos.FenceDurability.APPLIED, timeout=2.0)
            print("SITOS_BUFFER_PUBLISHER_OK", flush=True)
        except Exception as error:
            # Sitos 0.1.3 rejects caller-supplied Zenoh configuration for BufferPublisher because
            # it cannot prove the Fence QoS profile. Keep the raw wire fallback visible as a spike
            # finding so the parameter/session path can still be exercised end to end.
            publisher_error = f"{type(error).__name__}: {error}"
            config = (
                zenoh.Config.from_json5(zenoh_config)
                if zenoh_config
                else zenoh.Config()
            )
            with zenoh.open(config) as raw:
                raw.put(
                    f"{prefix}/buffers/{session_id}/durable/{DURABLE_KEY}",
                    b"durable-value",
                )
                raw.put(
                    f"{prefix}/buffers/{session_id}/ephemeral/{EPHEMERAL_KEY}",
                    b"ephemeral-value",
                )
            print(f"SITOS_BUFFER_PUBLISHER_LIMITATION {publisher_error}", flush=True)

        if not read_durable_buffer(prefix, session_id, DURABLE_KEY, zenoh_config):
            raise RuntimeError("durable buffer was not readable")
        print(
            f"SITOS_WORKER_PARAMETER_OK input={input_value} output={output_value}",
            flush=True,
        )
        if args.sleep_after > 0:
            time.sleep(args.sleep_after)
        print(f"SITOS_WORKER_SCENARIO_OK exit_code={args.exit_code}", flush=True)
        return args.exit_code
    except Exception as error:  # the process status is part of the scenario
        print(
            f"SITOS_WORKER_ERROR prefix={prefix} sid={session_id} "
            f"zenoh_config_len={len(zenoh_config or '')} "
            f"{type(error).__name__}: {error}",
            file=sys.stderr,
            flush=True,
        )
        return UNEXPECTED_ERROR_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())
