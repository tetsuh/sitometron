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
for bad in abc 12abc -1 0 99999999999999999999; do
  status=0
  "$binary" --segment-limit "$bad" --journal "$maint" >"$scratch/bad-limit.log" 2>&1 || status=$?
  [[ "$status" -eq 2 ]] && grep -q -- '--segment-limit needs a positive number of bytes' "$scratch/bad-limit.log" || { echo "--segment-limit $bad: exit $status: $(cat "$scratch/bad-limit.log")"; exit 1; }
done
status=0
"$binary" --journal "$maint" --segment-limit >"$scratch/bad-limit.log" 2>&1 || status=$?
[[ "$status" -eq 2 ]] && grep -q -- '--segment-limit needs a positive number of bytes' "$scratch/bad-limit.log" || { echo "--segment-limit without a value: exit $status: $(cat "$scratch/bad-limit.log")"; exit 1; }
status=0
"$binary" --journal "$maint" --segment-limit '' >"$scratch/bad-limit.log" 2>&1 || status=$?
[[ "$status" -eq 2 ]] || { echo "--segment-limit '': exit $status"; exit 1; }
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

# A prune interrupted after its first move: the first Job spans 13 segments, so the retained
# Journal now holds records of a Job whose creation is archived. The daemon refuses it, and running
# prune again completes the interrupted prune from archive/.
first_segment=$(printf 'journal-%020d.ndjson' 1)
mkdir -m 700 "$maint/archive"
mv "$maint/$first_segment" "$maint/archive/"
if "$binary" --listen 127.0.0.1:0 --journal "$maint" >"$scratch/interrupted.log" 2>&1; then
  echo "expected the interrupted prune to be refused at startup"; exit 1
fi
grep -q 'never created' "$scratch/interrupted.log" || { echo "interrupted startup: $(cat "$scratch/interrupted.log")"; exit 1; }
pruned=$("$binary" journal prune --journal "$maint")
printf '%s\n' "$pruned" | grep -qx "already archived $first_segment" || { echo "prune: $pruned"; exit 1; }
[[ "$(printf '%s\n' "$pruned" | grep -c '^archived journal-')" -eq 25 ]] || { echo "prune: $pruned"; exit 1; }
printf '%s\n' "$pruned" | grep -qx "pruned job ${ids[0]}"
printf '%s\n' "$pruned" | grep -qx "pruned job ${ids[1]}"
printf '%s\n' "$pruned" | grep -q 'journal_pruned: 25 segments, 25 records, 2 Jobs; replay starts at 27; completes an interrupted prune after 1 archived segments'
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
# Malformed tool invocations are usage errors (exit 2) that touch no Journal.
for args in "prune --journal" "prune --journal --dry-run" "prune --dry-run --journal" "quarantine-tail --journal ''" \
    "prune" "quarantine-tail --dry-run --journal $maint" "prune --journal $maint extra" "bogus --journal $maint" ""; do
  status=0
  eval "\"\$binary\" journal $args" >"$scratch/tool.log" 2>&1 || status=$?
  [[ "$status" -eq 2 ]] && grep -q 'usage: sitometron_spike journal' "$scratch/tool.log" || { echo "journal $args: exit $status: $(cat "$scratch/tool.log")"; exit 1; }
done
[[ ! -e ./--dry-run ]] || { echo "an option was taken as a Journal directory"; exit 1; }
start_on "$maint" --segment-limit 1
curl -fsS "$base/healthz" | grep -q '"ready":true'
kill -TERM "$daemon"
wait "$daemon"

# A torn tail after the last possible sequence: quarantine-tail cuts it and reports that no next
# sequence exists, and startup then refuses the exhausted Journal.
exhausted="$scratch/exhausted"
mkdir -p "$exhausted"
last_segment="$exhausted/journal-18446744073709551615.ndjson"
head -n1 "$maint/archive/$(printf 'journal-%020d.ndjson' 1)" |
  sed 's/"sequence":1,/"sequence":18446744073709551615,/' >"$last_segment"
grep -q '"sequence":18446744073709551615,' "$last_segment" || { echo "could not build the exhausted record"; exit 1; }
printf '{"seq' >>"$last_segment"
exhausted_out=$("$binary" journal quarantine-tail --journal "$exhausted")
printf '%s\n' "$exhausted_out" | grep -qx 'sequence exhausted: the daemon will refuse to start' || { echo "exhausted quarantine: $exhausted_out"; exit 1; }
printf '%s\n' "$exhausted_out" | grep -q 'next sequence' && { echo "an exhausted Journal reported a next sequence: $exhausted_out"; exit 1; }
printf '%s\n' "$exhausted_out" | grep -q "; sequence exhausted in $exhausted"
if "$binary" --listen 127.0.0.1:0 --journal "$exhausted" >"$scratch/exhausted-start.log" 2>&1; then
  echo "expected the exhausted Journal to be refused"; exit 1
fi
grep -q 'exhausted' "$scratch/exhausted-start.log" || { echo "no exhaustion refusal: $(cat "$scratch/exhausted-start.log")"; exit 1; }

# External REST v1 prototype (Accepted ADR-0008): create, read, list, health, readiness, and the
# error envelope. The client names a registered Application; it never sends an executable.
v1="$scratch/v1"
start_on "$v1" --max-jobs 2 --application ok='echo v1-ok' --application bad='exit 3' \
  --application slow='sleep 30'
# Sends one request and prints "<status> <body>". Arguments are passed to curl.
call() { curl -sS -o "$scratch/v1.body" -w '%{http_code}' "$@"; printf ' %s' "$(cat "$scratch/v1.body")"; }
# expect <label> <status> <text the body must contain> <curl arguments...>
expect() {
  local label=$1 status=$2 needle=$3 answer
  shift 3
  answer=$(call "$@")
  [[ "${answer%% *}" == "$status" ]] || { echo "v1 $label: expected $status: $answer"; exit 1; }
  [[ "$answer" == *"$needle"* ]] || { echo "v1 $label: expected $needle: $answer"; exit 1; }
  no_leak "$label" "$answer"
}
# No /v1 response carries an executable, a file-system path, or a raw parser or system error text.
no_leak() {
  local label=$1 answer=$2 text
  for text in executable /bin/ /tmp/ /home/ "$scratch" 'parse error' 'json.exception' \
    'syntax error' 'No such file' 'Permission denied' errno; do
    [[ "$answer" != *"$text"* ]] || { echo "v1 $label: the response leaks '$text': $answer"; exit 1; }
  done
}
# A successful /v1 request whose body the test parses: prints the body after the same leak check.
v1() {
  local out
  out=$(curl -fsS "$@")
  no_leak direct "$out"
  printf '%s' "$out"
}
envelope() { printf '"error":{"domain":"%s","code":"%s",' "$1" "$2"; }
json='Content-Type: application/json'
# Journal records of the /v1 daemon so far.
v1_records() { { cat "$v1"/journal-*.ndjson 2>/dev/null || true; } | wc -l; }
before=$(v1_records)

expect health 200 '{"status":"ok"}' "$base/v1/health"
expect ready 200 '{"ready":true,"reasons":[]}' "$base/v1/ready"
expect empty-list 200 '{"jobs":[]}' "$base/v1/jobs"

# Every request and job error of ADR-0008 Section 5 that this increment can produce. None of them
# reaches the writer, so the Journal does not grow.
expect route 404 "$(envelope request route_not_found)" "$base/v1/nothing"
expect bare-prefix 404 "$(envelope request route_not_found)" "$base/v1"
expect below-job 404 "$(envelope request route_not_found)" -X POST \
  "$base/v1/jobs/01890f3e-7b00-7abc-8abc-000000000001/terminate"
expect method-jobs 405 "$(envelope request method_not_allowed)" -X DELETE "$base/v1/jobs"
expect method-job 405 "$(envelope request method_not_allowed)" -X POST \
  "$base/v1/jobs/01890f3e-7b00-7abc-8abc-000000000001"
expect method-options 405 "$(envelope request method_not_allowed)" -X OPTIONS "$base/v1/jobs"
expect method-health 405 "$(envelope request method_not_allowed)" -X POST "$base/v1/health"
allow=$(curl -sS -D - -o /dev/null -X DELETE "$base/v1/jobs")
no_leak allow-headers "$allow"
printf '%s' "$allow" | grep -qi '^allow: GET, POST' || { echo "v1: 405 lacks the Allow header"; exit 1; }
expect media 415 "$(envelope request unsupported_media_type)" -X POST "$base/v1/jobs" \
  -H 'Content-Type: text/plain' -d '{"application_id":"ok"}'
expect malformed 400 "$(envelope request malformed_json)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":'
expect no-body 400 "$(envelope request malformed_json)" -X POST "$base/v1/jobs"
expect duplicate-key 400 "$(envelope request malformed_json)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":"ok","application_id":"ok"}'
# A raw NUL is not JSON, wherever it is: the text before it must not be accepted on its own.
nul_case() {
  local label=$1
  expect "$label" 400 "$(envelope request malformed_json)" -X POST "$base/v1/jobs" -H "$json" \
    --data-binary "@$scratch/v1.nul"
}
printf '{"application_id":"ok"}\0' >"$scratch/v1.nul"
nul_case nul-after-object
printf '{"application_id":"ok"} \n\0' >"$scratch/v1.nul"
nul_case nul-after-whitespace
printf '{"application_id":"ok"}\0{"executable":"/bin/true"}' >"$scratch/v1.nul"
nul_case nul-before-trailing-data
printf '{"application_id":"o\0k"}' >"$scratch/v1.nul"
nul_case nul-inside-string
printf '\0{"application_id":"ok"}' >"$scratch/v1.nul"
nul_case nul-first
expect trailing-data 400 "$(envelope request malformed_json)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":"ok"}{"application_id":"ok"}'
expect unknown-field 422 "$(envelope request validation_failed)" -X POST "$base/v1/jobs" \
  -H "$json" -d '{"application_id":"ok","executable":"/bin/true"}'
expect missing-field 422 "$(envelope request validation_failed)" -X POST "$base/v1/jobs" \
  -H "$json" -d '{}'
expect wrong-type 422 "$(envelope request validation_failed)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":7}'
expect not-object 422 "$(envelope request validation_failed)" -X POST "$base/v1/jobs" -H "$json" \
  -d '["ok"]'
expect empty-id 422 "$(envelope request validation_failed)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":""}'
expect path-in-malformed 400 "$(envelope request malformed_json)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":"/bin/true"'
expect path-as-application 422 "$(envelope job unknown_application)" -X POST "$base/v1/jobs" \
  -H "$json" -d '{"application_id":"/bin/true"}'
expect unknown-application 422 "$(envelope job unknown_application)" -X POST "$base/v1/jobs" \
  -H "$json" -d '{"application_id":"nobody"}'
expect query-on-prefix 404 "$(envelope request route_not_found)" "$base/v1?x=1"
expect query-on-health 404 "$(envelope request route_not_found)" "$base/v1/health?x=1"
expect query-on-list 404 "$(envelope request route_not_found)" "$base/v1/jobs?limit=1"
expect query-on-job 404 "$(envelope request route_not_found)" \
  "$base/v1/jobs/01890f3e-7b00-7abc-8abc-000000000001?x=1"
expect invalid-id 400 "$(envelope request invalid_job_id)" "$base/v1/jobs/not-a-uuid"
expect upper-id 400 "$(envelope request invalid_job_id)" \
  "$base/v1/jobs/01890F3E-7B00-7ABC-8ABC-000000000001"
expect unknown-job 404 "$(envelope job job_not_found)" \
  "$base/v1/jobs/01890f3e-7b00-7abc-8abc-000000000001"
# A request that stops arriving is answered 408 after the listener's receive timeout.
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /v1/health HTTP/1.1\r\n' >&3
late=$(timeout 10 cat <&3 || true)
exec 3<&- 3>&-
[[ "$late" == 'HTTP/1.1 408 '* && "$late" == *"$(envelope request request_timeout)"* ]] ||
  { echo "v1 timeout: expected 408 request_timeout: $late"; exit 1; }
no_leak timeout "$late"
# A request line that is not "METHOD /target HTTP/1.0|1.1" is refused before any route is known.
expect malformed-http 400 "$(envelope request malformed_request)" -X 'BAD REQUEST' "$base/v1/health"
# raw <label> <request line>: sends one raw request and expects 400 malformed_request.
raw() {
  local label=$1 line=$2 answer
  exec 3<>"/dev/tcp/127.0.0.1/$port"
  printf '%s\r\n\r\n' "$line" >&3
  answer=$(timeout 10 cat <&3 || true)
  exec 3<&- 3>&-
  [[ "$answer" == 'HTTP/1.1 400 '* && "$answer" == *"$(envelope request malformed_request)"* ]] ||
    { echo "v1 $label: expected 400 malformed_request: $answer"; exit 1; }
  no_leak "$label" "$answer"
}
raw bad-version 'GET /v1/health HTTP/1.foo'
raw other-version 'GET /v1/health HTTP/2.0'
raw trailing-token 'GET /v1/health HTTP/1.1 extra'
raw no-version 'GET /v1/health'
raw relative-target 'GET v1/health HTTP/1.1'
raw absolute-target 'GET http://127.0.0.1/v1/health HTTP/1.1'
# The same request line in its valid form is served.
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /v1/health HTTP/1.0\r\n\r\n' >&3
[[ "$(timeout 10 cat <&3 || true)" == 'HTTP/1.1 200 '*'{"status":"ok"}' ]] || { echo "v1: HTTP/1.0 health refused"; exit 1; }
exec 3<&- 3>&-
head -c 70000 /dev/zero | tr '\0' 'a' >"$scratch/v1.big"
expect too-large 413 "$(envelope request payload_too_large)" -X POST "$base/v1/jobs" -H "$json" \
  --data-binary "@$scratch/v1.big"
[[ "$(v1_records)" == "$before" ]] || { echo "v1: a refused request reached the Journal"; exit 1; }

# Create answers 202 with the Job resource and its Location; the Job is at least admitted.
headers=$(curl -sS -D - -o "$scratch/v1.body" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":"ok"}')
created=$(cat "$scratch/v1.body")
printf '%s' "$headers" | head -n1 | grep -q ' 202 ' || { echo "v1 create: $headers $created"; exit 1; }
v1_ok=$(printf '%s' "$created" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
[[ -n "$v1_ok" ]] || { echo "v1 create: no job_id in $created"; exit 1; }
printf '%s' "$headers" | tr -d '\r' | grep -qix "location: /v1/jobs/$v1_ok" ||
  { echo "v1 create: no Location for $v1_ok: $headers"; exit 1; }
# The Job is at least admitted; a fast command may already be terminal when the response is built.
[[ "$created" == "{\"job_id\":\"$v1_ok\",\"outcome\":"*"\"state\":\""*"\"terminal\":"* ]] ||
  { echo "v1 create: unexpected resource: $created"; exit 1; }
no_leak create "$created"
no_leak create-headers "$headers"

# Polls a Job until it is terminal, and prints its resource.
v1_terminal() {
  local id=$1 body=''
  for _ in $(seq 1 200); do
    body=$(v1 "$base/v1/jobs/$id")
    if [[ "$body" == *'"terminal":true'* ]]; then printf '%s' "$body"; return 0; fi
    read -r -t 0.05 <> <(:) || true
  done
  echo "v1 job $id did not become terminal: $body" >&2
  return 1
}
done_ok=$(v1_terminal "$v1_ok")
[[ "$done_ok" == "{\"job_id\":\"$v1_ok\",\"outcome\":\"succeeded\",\"state\":\"succeeded\",\"terminal\":true}" ]] ||
  { echo "v1 read: unexpected resource: $done_ok"; exit 1; }
v1_bad=$(v1 -X POST "$base/v1/jobs" -H "$json; charset=utf-8" -d '{"application_id":"bad"}' |
  sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
done_bad=$(v1_terminal "$v1_bad")
[[ "$done_bad" == *'"outcome":"failed"'*'"state":"failed"'* ]] || { echo "v1 read: $done_bad"; exit 1; }
# The list holds every resident Job in creation order.
listing=$(v1 "$base/v1/jobs")
[[ "$listing" == "{\"jobs\":[{\"job_id\":\"$v1_ok\","*"{\"job_id\":\"$v1_bad\","* ]] ||
  { echo "v1 list: $listing"; exit 1; }
# Both resident slots are used: creation is refused at once, and nothing is written.
before=$(v1_records)
expect capacity 503 "$(envelope service capacity_exhausted)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":"ok"}'
[[ "$(v1_records)" == "$before" ]] || { echo "v1: a refused creation reached the Journal"; exit 1; }
kill -TERM "$daemon"
wait "$daemon"
# The Journal records the registered identifier, not one derived from the shell.
for app in ok bad; do
  grep -h '"worker_launch_intent"' "$v1"/journal-*.ndjson | grep -q "\"application_id\":\"$app\"" ||
    { echo "v1: launch intent lacks application_id $app"; exit 1; }
done
# An identifier that is not lowercase ASCII in the stable-identifier form is refused at startup.
for bad_id in 'UPPER=true' '.dot=true' 'sp ace=true' '=true' 'noequals' 'empty='; do
  if "$binary" --listen 127.0.0.1:0 --journal "$scratch/v1-unused" --application "$bad_id" \
    >"$scratch/v1-option.log" 2>&1; then
    echo "expected --application $bad_id to be refused"; exit 1
  fi
  grep -q -- '--application needs ID=COMMAND' "$scratch/v1-option.log" ||
    { echo "no option error for $bad_id: $(cat "$scratch/v1-option.log")"; exit 1; }
done
[[ ! -e "$scratch/v1-unused" ]] || { echo "a refused option created a Journal"; exit 1; }

# Cancel (ADR-0008 Section 3). "lingering" takes a second to stop, so a repeated cancel finds the
# Job still stopping; "prompt" stops at once.
v1cancel="$scratch/v1cancel"
start_on "$v1cancel" --cancel-principal smoke-operator \
  --application lingering='trap "sleep 1; exit 0" TERM; while :; do sleep 0.05; done' \
  --application prompt='trap "exit 0" TERM; while :; do sleep 0.05; done'
# Creates a Job for the application and waits until it is running; prints its identifier.
v1_running() {
  local id body=''
  id=$(v1 -X POST "$base/v1/jobs" -H "$json" -d "{\"application_id\":\"$1\"}" |
    sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
  for _ in $(seq 1 200); do
    body=$(v1 "$base/v1/jobs/$id")
    if [[ "$body" == *'"state":"running"'* ]]; then printf '%s' "$id"; return 0; fi
    read -r -t 0.05 <> <(:) || true
  done
  echo "v1 job $id did not start running: $body" >&2
  return 1
}
lingering=$(v1_running lingering)
expect cancel 202 "{\"job_id\":\"$lingering\",\"outcome\":null,\"state\":\"stopping\",\"terminal\":false}" \
  -X POST "$base/v1/jobs/$lingering/cancel"
expect cancel-repeated 409 "$(envelope job stop_cause_already_latched)" \
  -X POST "$base/v1/jobs/$lingering/cancel"
cancelled="{\"job_id\":\"$lingering\",\"outcome\":\"cancelled\",\"state\":\"cancelled\",\"terminal\":true}"
[[ "$(v1_terminal "$lingering")" == "$cancelled" ]] ||
  { echo "v1 cancel: unexpected resource: $(v1 "$base/v1/jobs/$lingering")"; exit 1; }
expect cancel-terminal 409 "$(envelope job command_not_allowed_in_state)" \
  -X POST "$base/v1/jobs/$lingering/cancel"
# The body is optional; an empty object is the only body accepted.
prompt=$(v1_running prompt)
expect cancel-empty-object 202 "\"job_id\":\"$prompt\"" -X POST "$base/v1/jobs/$prompt/cancel" \
  -H "$json" -d '{}'
[[ "$(v1_terminal "$prompt")" == *'"outcome":"cancelled"'* ]] || { echo "v1 cancel: prompt Job not cancelled"; exit 1; }
# Refusals: none of them records a cancel.
cancels() { { grep -h '"event_type":"cancel_accepted"' "$v1cancel"/journal-*.ndjson || true; } | wc -l; }
[[ "$(cancels)" == 2 ]] || { echo "v1 cancel: expected two cancel_accepted records, got $(cancels)"; exit 1; }
expect cancel-unknown 404 "$(envelope job job_not_found)" \
  -X POST "$base/v1/jobs/01890000-0000-7000-8000-000000000000/cancel"
expect cancel-invalid-id 400 "$(envelope request invalid_job_id)" \
  -X POST "$base/v1/jobs/not-a-uuid/cancel"
expect cancel-method 405 "$(envelope request method_not_allowed)" "$base/v1/jobs/$prompt/cancel"
allow=$(curl -sS -D - -o /dev/null "$base/v1/jobs/$prompt/cancel")
no_leak cancel-allow-headers "$allow"
printf '%s' "$allow" | grep -qi '^Allow: POST' || { echo "v1 cancel: 405 without Allow: POST: $allow"; exit 1; }
expect cancel-media 415 "$(envelope request unsupported_media_type)" \
  -X POST "$base/v1/jobs/$prompt/cancel" -H 'Content-Type: text/plain' -d '{}'
expect cancel-malformed 400 "$(envelope request malformed_json)" \
  -X POST "$base/v1/jobs/$prompt/cancel" -H "$json" -d '{'
expect cancel-body 422 "$(envelope request validation_failed)" \
  -X POST "$base/v1/jobs/$prompt/cancel" -H "$json" -d '{"reason":"x"}'
expect cancel-below 404 "$(envelope request route_not_found)" \
  -X POST "$base/v1/jobs/$prompt/cancel/again"
expect cancel-query 404 "$(envelope request route_not_found)" \
  -X POST "$base/v1/jobs/$prompt/cancel?force=1"
[[ "$(cancels)" == 2 ]] || { echo "v1 cancel: a refused cancel reached the Journal"; exit 1; }
# Both records carry the configured principal and nothing else.
[[ "$(grep -h '"event_type":"cancel_accepted"' "$v1cancel"/journal-*.ndjson |
  grep -c '"payload":{"principal_subject":"smoke-operator"}')" == 2 ]] ||
  { echo "v1 cancel: cancel_accepted lacks the configured principal"; exit 1; }
# A cancel sent right after the creation overtakes the driver at whatever step it has reached
# (before or after the launch). Wherever it lands, the Job ends cancelled without a driver error.
early_steps=''
for _ in 1 2 3 4 5; do
  early=$(v1 -X POST "$base/v1/jobs" -H "$json" -d '{"application_id":"prompt"}' |
    sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
  expect cancel-early 202 "\"job_id\":\"$early\"" -X POST "$base/v1/jobs/$early/cancel"
  [[ "$(v1_terminal "$early")" == *'"outcome":"cancelled"'* ]] || { echo "v1 cancel: early Job not cancelled"; exit 1; }
  for _ in $(seq 1 100); do
    early_body=$(curl -fsS "$base/jobs/$early")
    [[ "$early_body" == *'"cleanup_status_recorded"'* ]] && break
    read -r -t 0.05 <> <(:) || true
  done
  [[ "$early_body" == *'"cleanup_status_recorded"'*'"error":null'* || "$early_body" == *'"error":null'*'"cleanup_status_recorded"'* ]] ||
    { echo "v1 cancel: the driver did not finish the early Job cleanly: $early_body"; exit 1; }
  # A Job that had a launch confirms its exit once; one cancelled before the launch intent has no
  # launch to confirm.
  steps=$(sed -n 's/.*"steps":\[\([^]]*\)\].*/\1/p' <<<"$early_body")
  confirmations=$({ grep -o '"process_exit_confirmed"' <<<"$steps" || true; } | wc -l)
  if [[ "$steps" == *'"worker_launch_intent"'* ]]; then
    [[ "$confirmations" == 1 ]] || { echo "v1 cancel: $confirmations exit confirmations: $early_body"; exit 1; }
  else
    [[ "$confirmations" == 0 ]] || { echo "v1 cancel: an exit was confirmed without a launch: $early_body"; exit 1; }
  fi
  early_steps+="$steps"$'\n'
done
# Which landing points this run reached is timing; they are printed for the record.
printf 'early cancel steps:\n%s' "$early_steps"
kill -TERM "$daemon"
wait "$daemon"
# A principal that is empty or not visible ASCII is refused at startup.
for bad_principal in '' 'two words'; do
  if "$binary" --listen 127.0.0.1:0 --journal "$scratch/v1-unused" \
    --cancel-principal "$bad_principal" >"$scratch/v1-option.log" 2>&1; then
    echo "expected --cancel-principal '$bad_principal' to be refused"; exit 1
  fi
  grep -q -- '--cancel-principal needs a name' "$scratch/v1-option.log" ||
    { echo "no option error for '$bad_principal': $(cat "$scratch/v1-option.log")"; exit 1; }
done
[[ ! -e "$scratch/v1-unused" ]] || { echo "a refused option created a Journal"; exit 1; }

# kill -9 during a Job: after the restart readiness is false with the unresolved Job, creation is
# refused with not_ready, and the Job is still readable.
v1crash="$scratch/v1crash"
start_on "$v1crash" --application slow='sleep 30'
v1_slow=$(v1 -X POST "$base/v1/jobs" -H "$json" -d '{"application_id":"slow"}' |
  sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p')
child=''
for _ in $(seq 1 200); do
  body=$(v1 "$base/v1/jobs/$v1_slow")
  if [[ "$body" == *'"state":"running"'* ]]; then
    child=$(curl -fsS "$base/jobs/$v1_slow" | sed -n 's/.*"pid":\([0-9]*\).*/\1/p')
    break
  fi
  read -r -t 0.05 <> <(:) || true
done
[[ -n "$child" ]] || { echo "v1 crash job did not start running: $body"; exit 1; }
kill -KILL "$daemon"
wait "$daemon" 2>/dev/null || true
kill -KILL "$child" 2>/dev/null || true
start_on "$v1crash" --application slow='sleep 30'
expect not-ready 503 "{\"ready\":false,\"reasons\":[{\"code\":\"unresolved_jobs\",\"job_ids\":[\"$v1_slow\"]}]}" \
  "$base/v1/ready"
expect health-while-not-ready 200 '{"status":"ok"}' "$base/v1/health"
expect create-not-ready 503 "$(envelope service not_ready)" -X POST "$base/v1/jobs" -H "$json" \
  -d '{"application_id":"slow"}'
# A cancel is a normal ingress input: while admission is closed it is refused like a creation.
expect cancel-not-ready 503 "$(envelope service not_ready)" \
  -X POST "$base/v1/jobs/$v1_slow/cancel"
expect read-recovered 200 "{\"job_id\":\"$v1_slow\",\"outcome\":null,\"state\":\"running\",\"terminal\":false}" \
  "$base/v1/jobs/$v1_slow"
kill -TERM "$daemon"
wait "$daemon"

trap - EXIT
echo "smoke ok: $lines journal lines, port $port"
exit 0
