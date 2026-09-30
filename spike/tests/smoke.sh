#!/usr/bin/env bash
# Smoke test for the walking skeleton (Issue #48): start the daemon on an ephemeral port, submit
# one succeeding and one failing Job, wait for both to reach a terminal state, and read the
# Journal back. Requires curl. Registered under CTest only when SITOMETRON_BUILD_SPIKE=ON.
set -Eeuo pipefail

binary=$1
scratch=$2
rm -rf -- "$scratch"
mkdir -p -- "$scratch"
journal="$scratch/journal"
# All records of the segmented Journal, in sequence order.
records() { cat "$journal"/journal-*.ndjson; }
log="$scratch/daemon.log"

command -v curl >/dev/null || { echo "curl is required"; exit 2; }

# Non-loopback listen addresses are refused before anything is bound.
if "$binary" --listen 0.0.0.0:0 --journal "$scratch/unused" >"$scratch/refused.log" 2>&1; then
  echo "expected non-loopback listen to be refused"; exit 1
fi
grep -q 'loopback' "$scratch/refused.log"

# An existing Journal whose active segment ends in a torn record is refused rather than appended onto.
mkdir -p "$scratch/torn"
printf '{"sequence":1}' >"$scratch/torn/journal-00000000000000000001.ndjson"
if "$binary" --listen 127.0.0.1:0 --journal "$scratch/torn" >"$scratch/torn.log" 2>&1; then
  echo "expected a torn journal to be refused"; exit 1
fi
grep -q 'torn' "$scratch/torn.log"

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
lines=$(records | wc -l)
[[ "$lines" -eq 26 ]] || { echo "expected 26 journal lines, got $lines"; records; exit 1; }
records | grep -c "\"job_id\":\"$ok_id\"" | grep -qx 13
records | grep -c "\"job_id\":\"$bad_id\"" | grep -qx 13
grep -q '"event_type":"worker_failed"' < <(records)
grep -q '"outcome":"succeeded"' < <(records)

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

# A malformed Content-Length is rejected with 400 and never starts a Job.
before=$(curl -fsS "$base/jobs" | grep -o '"job_id"' | wc -l)
malformed=$(
  trap '' PIPE
  exec 3<>"/dev/tcp/127.0.0.1/$port" || exit 0
  printf 'POST /jobs HTTP/1.1\r\nHost: x\r\nContent-Length: 25junk\r\n\r\n{"executable":"/bin/true"}' >&3
  timeout 5 head -c 200 <&3 || true
  exec 3>&- 3<&- || true
)
printf '%s' "$malformed" | grep -q '^HTTP/1.1 400' || { echo "expected 400 for malformed Content-Length: $malformed"; exit 1; }
after=$(curl -fsS "$base/jobs" | grep -o '"job_id"' | wc -l)
[[ "$before" -eq "$after" ]] || { echo "malformed request started a Job"; exit 1; }

# A client that trickles bytes is cut off by the total request deadline (408 or closed socket),
# and the server keeps serving afterwards.
trickle_start=$(date +%s)
(
  trap '' PIPE
  exec 3<>"/dev/tcp/127.0.0.1/$port" || exit 0
  for _ in $(seq 1 12); do printf 'G' >&3 2>/dev/null || break; read -r -t 1 <> <(:) || true; done
  exec 3>&- 3<&- || true
) || true
trickle_elapsed=$(( $(date +%s) - trickle_start ))
[[ "$trickle_elapsed" -le 10 ]] || { echo "trickle client was not cut off: ${trickle_elapsed}s"; exit 1; }
curl -fsS "$base/healthz" | grep -q '"status":"ok"'

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
lines=$(records | wc -l)
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
grep -q 'next sequence: 64, replayed jobs: 5, unresolved: 0' "$log"
# Every Job of the previous run was closed, so the restarted daemon is ready.
curl -fsS "$base/healthz" | grep -q '"ready":true'
curl -fsS "$base/jobs" | grep -o '"job_id"' | wc -l | grep -qx 5
curl -fsS "$base/jobs" | grep -o '"recovered":true' | wc -l | grep -qx 5
again=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' -d '{"executable":"/bin/true"}')
again_id=$(printf '%s' "$again" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
wait_terminal "$again_id" >/dev/null
listing=$(curl -fsS "$base/jobs")
printf '%s' "$listing" | grep -q "{\"job_id\":\"$again_id\",\"recovered\":false," || { echo "new Job not listed as fresh: $listing"; exit 1; }
printf '%s' "$listing" | grep -o '"recovered":true' | wc -l | grep -qx 5 || { echo "expected 5 recovered Jobs: $listing"; exit 1; }
kill -TERM "$daemon"
wait "$daemon"
lines=$(records | wc -l)
[[ "$lines" -eq 76 ]] || { echo "expected 76 journal lines after restart, got $lines"; exit 1; }
records | tail -n1 | grep -q '"sequence":76' || { echo "sequence did not continue: $(records | tail -n1)"; exit 1; }

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

# kill -9 while a Job runs: after restart the Job is visible from the Journal alone, it is reported
# unresolved, readiness is false, admission is closed, and the Journal is not modified.
crash="$scratch/crash"
# Starts the daemon on Journal $1 with any further options, and sets $daemon and $base.
start_on() {
  local dir=$1
  shift
  "$binary" --listen 127.0.0.1:0 --journal "$dir" --workdir "$scratch" "$@" >"$log" 2>&1 &
  daemon=$!
  port=''
  for _ in $(seq 1 100); do
    port=$(sed -n 's/.*listening on http:\/\/127\.0\.0\.1:\([0-9]*\).*/\1/p' "$log" | head -n1)
    [[ -n "$port" ]] && break
    read -r -t 0.1 <> <(:) || true
  done
  [[ -n "$port" ]] || { echo "daemon on $dir did not report a port"; cat "$log"; exit 1; }
  base="http://127.0.0.1:$port"
}
start_daemon() { start_on "$crash"; }
start_daemon
running=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' \
  -d '{"executable":"/bin/sh","args":["-c","sleep 30"]}')
running_id=$(printf '%s' "$running" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
child=''
for _ in $(seq 1 200); do
  body=$(curl -fsS "$base/jobs/$running_id")
  if printf '%s' "$body" | grep -q '"worker_running"'; then
    child=$(printf '%s' "$body" | sed -n 's/.*"pid":\([0-9]*\).*/\1/p')
    break
  fi
  read -r -t 0.05 <> <(:) || true
done
[[ -n "$child" ]] || { echo "crash job did not start running: $body"; exit 1; }
kill -KILL "$daemon"
wait "$daemon" 2>/dev/null || true
kill -KILL "$child" 2>/dev/null || true
crash_files=$(ls "$crash")
crash_bytes=$(cat "$crash"/journal-*.ndjson | cksum)

start_daemon
grep -q "unresolved job $running_id: admission closed" "$log" || { echo "unresolved job not reported"; cat "$log"; exit 1; }
grep -q 'replayed jobs: 1, unresolved: 1' "$log"
health=$(curl -sS "$base/healthz" -w '\n%{http_code}')
printf '%s' "$health" | tail -n1 | grep -qx 503 || { echo "expected 503 healthz: $health"; exit 1; }
printf '%s' "$health" | grep -q "\"unresolved\":\[\"$running_id\"\]" || { echo "healthz lacks $running_id: $health"; exit 1; }
recovered=$(curl -fsS "$base/jobs/$running_id")
printf '%s' "$recovered" | grep -q '"recovered":true' || { echo "not recovered: $recovered"; exit 1; }
printf '%s' "$recovered" | grep -q '"terminal":false' || { echo "expected non-terminal: $recovered"; exit 1; }
listing=$(curl -fsS "$base/jobs")
printf '%s' "$listing" | grep -q "{\"job_id\":\"$running_id\",\"recovered\":true," || { echo "listing lacks recovered $running_id: $listing"; exit 1; }
curl -sS -X POST "$base/jobs" -H 'Content-Type: application/json' -d '{"executable":"/bin/true"}' \
  -o /dev/null -w '%{http_code}\n' | grep -qx 503
kill -TERM "$daemon"
wait "$daemon"
[[ "$(ls "$crash")" == "$crash_files" ]] || { echo "journal files changed: $(ls "$crash")"; exit 1; }
[[ "$(cat "$crash"/journal-*.ndjson | cksum)" == "$crash_bytes" ]] || { echo "journal bytes changed"; exit 1; }

# Offline maintenance (OPS-005). With one record per segment, three Jobs run one after another
# leave segment boundaries between Jobs, so the first two Jobs' 26 segments form a closed prefix.
maint="$scratch/maint"
for bad in abc 12abc -1 99999999999999999999; do
  status=0
  "$binary" --segment-limit "$bad" --journal "$maint" >"$scratch/bad-limit.log" 2>&1 || status=$?
  [[ "$status" -eq 2 ]] && grep -q -- '--segment-limit needs a number of bytes' "$scratch/bad-limit.log" || { echo "--segment-limit $bad: exit $status: $(cat "$scratch/bad-limit.log")"; exit 1; }
done
[[ ! -e "$maint" ]] || { echo "a rejected option created the Journal"; exit 1; }
submit_true() {
  local body
  body=$(curl -fsS -X POST "$base/jobs" -H 'Content-Type: application/json' -d '{"executable":"/bin/true"}')
  printf '%s' "$body" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p'
}
start_on "$maint" --segment-limit 1
ids=()
for _ in 1 2 3; do
  id=$(submit_true)
  wait_terminal "$id" >/dev/null
  ids+=("$id")
done
# While the daemon holds the Journal lock, both tools refuse and change nothing.
for tool in "prune" "quarantine-tail"; do
  if "$binary" journal $tool --journal "$maint" >"$scratch/tool.log" 2>&1; then
    echo "journal $tool ran while the daemon held the lock"; exit 1
  fi
  grep -q 'journal_locked' "$scratch/tool.log" || { echo "expected journal_locked: $(cat "$scratch/tool.log")"; exit 1; }
done
kill -TERM "$daemon"
wait "$daemon"
[[ "$(ls "$maint"/journal-*.ndjson | wc -l)" -eq 39 ]] || { echo "expected 39 segments: $(ls "$maint")"; exit 1; }
maint_files=$(ls "$maint")
maint_bytes=$(cat "$maint"/journal-*.ndjson | cksum)

dry=$("$binary" journal prune --journal "$maint" --dry-run)
[[ "$(printf '%s\n' "$dry" | grep -c '^would archive journal-')" -eq 26 ]] || { echo "dry run: $dry"; exit 1; }
printf '%s\n' "$dry" | grep -qx "would prune job ${ids[0]}"
printf '%s\n' "$dry" | grep -qx "would prune job ${ids[1]}"
printf '%s\n' "$dry" | grep -q "${ids[2]}" && { echo "dry run would prune the last Job: $dry"; exit 1; }
printf '%s\n' "$dry" | grep -q 'journal_prune_planned: 26 segments, 26 records, 2 Jobs; replay starts at 27'
[[ "$(ls "$maint")" == "$maint_files" && "$(cat "$maint"/journal-*.ndjson | cksum)" == "$maint_bytes" ]] || { echo "dry run changed the Journal"; exit 1; }

pruned=$("$binary" journal prune --journal "$maint")
[[ "$(printf '%s\n' "$pruned" | grep -c '^archived journal-')" -eq 26 ]] || { echo "prune: $pruned"; exit 1; }
printf '%s\n' "$pruned" | grep -qx "pruned job ${ids[0]}"
printf '%s\n' "$pruned" | grep -qx "pruned job ${ids[1]}"
printf '%s\n' "$pruned" | grep -q 'journal_pruned: 26 segments'
[[ "$(ls "$maint/archive" | wc -l)" -eq 26 && "$(ls "$maint"/journal-*.ndjson | wc -l)" -eq 13 ]] || { echo "unexpected layout: $(ls -R "$maint")"; exit 1; }
[[ "$(cat "$maint/archive"/journal-*.ndjson "$maint"/journal-*.ndjson | cksum)" == "$maint_bytes" ]] || { echo "pruning changed record bytes"; exit 1; }

# The restarted daemon replays only the retained Job, is ready, and continues the sequence.
start_on "$maint" --segment-limit 1
grep -q 'next sequence: 40, replayed jobs: 1, unresolved: 0' "$log" || { echo "restart after prune: $(cat "$log")"; exit 1; }
curl -fsS "$base/healthz" | grep -q '"ready":true'
listing=$(curl -fsS "$base/jobs")
printf '%s' "$listing" | grep -q "{\"job_id\":\"${ids[2]}\",\"recovered\":true," || { echo "retained Job missing: $listing"; exit 1; }
for gone in "${ids[0]}" "${ids[1]}"; do
  printf '%s' "$listing" | grep -q "$gone" && { echo "pruned Job $gone still listed: $listing"; exit 1; }
done
fresh=$(submit_true)
wait_terminal "$fresh" >/dev/null
grep -q "\"sequence\":40,.*\"job_id\":\"$fresh\"" "$maint/$(printf 'journal-%020d.ndjson' 40)" || { echo "the new Job does not start at sequence 40"; exit 1; }
kill -TERM "$daemon"
wait "$daemon"

# A torn tail: startup refuses with journal_torn_tail, quarantine-tail moves the bytes, and the
# daemon starts again.
highest=$(ls "$maint"/journal-*.ndjson | sort | tail -n1)
highest_bytes=$(cksum <"$highest")
printf '{"sequence":' >>"$highest"
if "$binary" --listen 127.0.0.1:0 --journal "$maint" >"$scratch/torn-start.log" 2>&1; then
  echo "expected the torn Journal to be refused"; exit 1
fi
grep -q 'journal_torn_tail' "$scratch/torn-start.log" || { echo "no journal_torn_tail: $(cat "$scratch/torn-start.log")"; exit 1; }
quarantined=$("$binary" journal quarantine-tail --journal "$maint")
printf '%s\n' "$quarantined" | grep -q "^quarantined 12 bytes of $(basename "$highest") from byte " || { echo "quarantine: $quarantined"; exit 1; }
[[ "$(cksum <"$highest")" == "$highest_bytes" ]] || { echo "the segment was not cut back to its last record"; exit 1; }
[[ "$(cat "$highest".torn-*)" == '{"sequence":' ]] || { echo "quarantine file content: $(cat "$highest".torn-*)"; exit 1; }
"$binary" journal quarantine-tail --journal "$maint" | grep -q 'journal_clean'
start_on "$maint" --segment-limit 1
curl -fsS "$base/healthz" | grep -q '"ready":true'
kill -TERM "$daemon"
wait "$daemon"
trap - EXIT
echo "smoke ok: $lines journal lines, port $port"
exit 0
