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

# Non-loopback listen addresses are refused before anything is bound.
if "$binary" --listen 0.0.0.0:0 --journal "$scratch/unused.jsonl" >"$scratch/refused.log" 2>&1; then
  echo "expected non-loopback listen to be refused"; exit 1
fi
grep -q 'loopback' "$scratch/refused.log"

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

# Journal so far: 13 records per Job (job_created .. cleanup_status_recorded).
lines=$(wc -l <"$journal")
[[ "$lines" -eq 26 ]] || { echo "expected 26 journal lines, got $lines"; cat "$journal"; exit 1; }
grep -c "\"job_id\":\"$ok_id\"" "$journal" | grep -qx 13
grep -c "\"job_id\":\"$bad_id\"" "$journal" | grep -qx 13
grep -q '"event_type":"worker_failed"' "$journal"
grep -q '"outcome":"succeeded"' "$journal"

curl -sS -X POST "$base/jobs" -d '{"executable":""}' -o /dev/null -w '%{http_code}\n' | grep -qx 400
curl -sS -X POST "$base/jobs" -d '{"executable":"/bin/true","workdir":7}' -o /dev/null -w '%{http_code}\n' | grep -qx 400

# A child killed by a signal is a failed Worker with the signal recorded.
sig=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' \
  -d '{"executable":"/bin/sh","args":["-c","kill -9 $$"]}')
sig_id=$(printf '%s' "$sig" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
sig_body=$(wait_terminal "$sig_id")
printf '%s\n' "$sig_body" | grep -q '"state":"failed"' || { echo "expected failed: $sig_body"; exit 1; }
printf '%s\n' "$sig_body" | grep -q '"signal":9' || { echo "expected signal 9: $sig_body"; exit 1; }

# Listing names every Job with its state.
listing=$(curl -fsS "$base/jobs")
for id in "$ok_id" "$bad_id" "$sig_id"; do
  printf '%s' "$listing" | grep -q "\"job_id\":\"$id\"" || { echo "listing lacks $id: $listing"; exit 1; }
done
printf '%s' "$listing" | grep -q '"state":"succeeded"'
printf '%s' "$listing" | grep -q '"state":"failed"'

# A working directory that cannot be entered is a spawn failure: launch observed as failed, no
# worker_running, and the Job still reaches a terminal state.
nodir=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' \
  -d "{\"executable\":\"/bin/true\",\"workdir\":\"$scratch/does-not-exist\"}")
nodir_id=$(printf '%s' "$nodir" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
nodir_body=$(wait_terminal "$nodir_id")
printf '%s\n' "$nodir_body" | grep -q '"state":"failed"' || { echo "expected failed: $nodir_body"; exit 1; }
printf '%s\n' "$nodir_body" | grep -q 'spawn failed' || { echo "expected spawn failure: $nodir_body"; exit 1; }
printf '%s\n' "$nodir_body" | grep -q '"worker_running"' && { echo "unexpected worker_running: $nodir_body"; exit 1; }

# A child that ignores SIGTERM must not block shutdown: it is killed after the grace period.
curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' \
  -d '{"executable":"/bin/sh","args":["-c","trap \"\" TERM; while :; do sleep 1; done"]}' >/dev/null
curl -sS "$base/jobs/nope" -o /dev/null -w '%{http_code}\n' | grep -qx 404

kill -TERM "$daemon"
wait "$daemon"
grep -q 'shutting down' "$log"
lines=$(wc -l <"$journal")
# 5 Jobs: 4 x 13 records plus the spawn-failure Job (11 records: no worker_running/worker_*).
[[ "$lines" -eq 63 ]] || { echo "expected 63 journal lines after shutdown, got $lines"; exit 1; }

# Restart on the same Journal: the logical sequence continues after the last durable record.
"$binary" --listen 127.0.0.1:0 --journal "$journal" --workdir "$scratch" >"$log" 2>&1 &
daemon=$!
port=''
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*listening on http:\/\/127\.0\.0\.1:\([0-9]*\).*/\1/p' "$log" | head -n1)
  [[ -n "$port" ]] && break
  read -r -t 0.1 <> <(:) || true
done
[[ -n "$port" ]] || { echo "restarted daemon did not report a port"; cat "$log"; exit 1; }
base="http://127.0.0.1:$port"
grep -q 'existing lines: 63' "$log"
again=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' -d '{"executable":"/bin/true"}')
again_id=$(printf '%s' "$again" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
wait_terminal "$again_id" >/dev/null
kill -TERM "$daemon"
wait "$daemon"
lines=$(wc -l <"$journal")
[[ "$lines" -eq 76 ]] || { echo "expected 76 journal lines after restart, got $lines"; exit 1; }
tail -n1 "$journal" | grep -q '"sequence":76' || { echo "sequence did not continue: $(tail -n1 "$journal")"; exit 1; }

# Immediate shutdown right after submission must not hang: the driver either refuses the launch
# or the shutdown sees the pid it has to signal.
"$binary" --listen 127.0.0.1:0 --journal "$journal" --workdir "$scratch" >"$log" 2>&1 &
daemon=$!
port=''
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*listening on http:\/\/127\.0\.0\.1:\([0-9]*\).*/\1/p' "$log" | head -n1)
  [[ -n "$port" ]] && break
  read -r -t 0.1 <> <(:) || true
done
base="http://127.0.0.1:$port"
curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' \
  -d '{"executable":"/bin/sh","args":["-c","sleep 30"]}' >/dev/null
kill -TERM "$daemon"
for _ in $(seq 1 150); do
  kill -0 "$daemon" 2>/dev/null || break
  read -r -t 0.1 <> <(:) || true
done
if kill -0 "$daemon" 2>/dev/null; then echo "daemon did not exit after immediate shutdown"; kill -KILL "$daemon"; exit 1; fi
wait "$daemon" || true
grep -q 'shutting down' "$log"
trap - EXIT
echo "smoke ok: $lines journal lines, port $port"
exit 0
