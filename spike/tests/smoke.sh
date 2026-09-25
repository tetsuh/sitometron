#!/usr/bin/env bash
# Smoke test for the walking skeleton (Issue #48): start the daemon on an ephemeral port, submit
# one succeeding and one failing Job, wait for both to reach a terminal state, and read the
# Journal back. Requires curl. Registered under CTest only when SITOMETRON_BUILD_SPIKE=ON.
set -Eeuo pipefail

binary=$1
scratch=$2
rm -rf -- "$scratch"
mkdir -p -- "$scratch"
journal="$scratch/journal.jsonl"
log="$scratch/daemon.log"

command -v curl >/dev/null || { echo "curl is required"; exit 2; }

"$binary" --listen 127.0.0.1:0 --journal "$journal" --workdir "$scratch" >"$log" 2>&1 &
daemon=$!
trap 'kill -TERM "$daemon" 2>/dev/null || true; wait "$daemon" 2>/dev/null || true' EXIT

port=''
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*listening on http:\/\/127\.0\.0\.1:\([0-9]*\).*/\1/p' "$log" | head -n1)
  [[ -n "$port" ]] && break
  read -r -t 0.1 <> <(:) || true
done
[[ -n "$port" ]] || { echo "daemon did not report a port"; cat "$log"; exit 1; }
base="http://127.0.0.1:$port"

curl -fsS "$base/healthz" | grep -q '"status":"ok"'

ok=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' \
  -d '{"executable":"/bin/sh","args":["-c","echo skeleton-ok"]}')
ok_id=$(printf '%s' "$ok" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[[ -n "$ok_id" ]] || { echo "no job_id in: $ok"; exit 1; }

bad=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' \
  -d '{"executable":"/bin/sh","args":["-c","exit 3"]}')
bad_id=$(printf '%s' "$bad" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[[ -n "$bad_id" ]] || { echo "no job_id in: $bad"; exit 1; }

wait_terminal() {
  local id=$1 body=''
  for _ in $(seq 1 200); do
    body=$(curl -fsS "$base/jobs/$id")
    # "terminal" flips at terminal_outcome_committed; the driver still appends process exit,
    # release, and cleanup afterwards, so wait for the last step instead.
    if printf '%s' "$body" | grep -q '"cleanup_status_recorded"'; then
      printf '%s' "$body"
      return 0
    fi
    read -r -t 0.05 <> <(:) || true
  done
  echo "job $id did not reach a terminal state: $body" >&2
  return 1
}

ok_body=$(wait_terminal "$ok_id")
printf '%s\n' "$ok_body" | grep -q '"state":"succeeded"' || { echo "expected succeeded: $ok_body"; exit 1; }
printf '%s\n' "$ok_body" | grep -q '"error":null' || { echo "unexpected error: $ok_body"; exit 1; }
printf '%s\n' "$ok_body" | grep -q '"cleanup_status_recorded"' || { echo "cleanup missing: $ok_body"; exit 1; }

bad_body=$(wait_terminal "$bad_id")
printf '%s\n' "$bad_body" | grep -q '"state":"failed"' || { echo "expected failed: $bad_body"; exit 1; }
printf '%s\n' "$bad_body" | grep -q '"exit_code":3' || { echo "expected exit 3: $bad_body"; exit 1; }

# Journal: 13 records per Job (job_created .. cleanup_status_recorded), all for known Jobs.
lines=$(wc -l <"$journal")
[[ "$lines" -eq 26 ]] || { echo "expected 26 journal lines, got $lines"; cat "$journal"; exit 1; }
grep -c "\"job_id\":\"$ok_id\"" "$journal" | grep -qx 13
grep -c "\"job_id\":\"$bad_id\"" "$journal" | grep -qx 13
grep -q '"event_type":"worker_failed"' "$journal"
grep -q '"outcome":"succeeded"' "$journal"

curl -sS -X POST "$base/jobs" -d '{"executable":""}' -o /dev/null -w '%{http_code}\n' | grep -qx 400
curl -sS "$base/jobs/nope" -o /dev/null -w '%{http_code}\n' | grep -qx 404

kill -TERM "$daemon"
wait "$daemon"
grep -q 'shutting down' "$log"
echo "smoke ok: $lines journal lines, port $port"
