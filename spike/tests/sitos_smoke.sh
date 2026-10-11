#!/usr/bin/env bash
# Opt-in Issue #98 smoke test. Requires the Sitos C++ package and the spike Python venv.
set -Eeuo pipefail

binary=$1
scratch=$2
python=$3
worker=$4
reader=$5
rm -rf -- "$scratch"
mkdir -p -- "$scratch"
journal="$scratch/journal"
log="$scratch/daemon.log"
prefix='sitometron-spike'
zenoh_config='{"mode":"client","connect":{"endpoints":["tcp/127.0.0.1:27400"]},"scouting":{"multicast":{"enabled":false}}}'
daemon=''

command -v curl >/dev/null || { echo "curl is required"; exit 2; }
[[ -x "$python" ]] || { echo "Python interpreter is not executable: $python"; exit 2; }
[[ -f "$worker" && -f "$reader" ]] || { echo "worker or reader script is missing"; exit 2; }

worker_command="exec \"$python\" \"$worker\""
failed_command="$worker_command --exit-code 3"
cancel_command="$worker_command --publish-delay 1 --sleep-after 30"
"$binary" --listen 127.0.0.1:0 --journal "$journal" --workdir "$scratch" \
  --application "success=$worker_command" \
  --application "failed=$failed_command" \
  --application "cancel=$cancel_command" >"$log" 2>&1 &
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

curl -fsS "$base/v1/health" | grep -q '"status":"ok"'

create_job() {
  local application=$1 body
  body=$(curl -fsS -X POST "$base/v1/jobs" -H 'Content-Type: application/json' \
    -d "{\"application_id\":\"$application\"}")
  printf '%s' "$body" | sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p'
}

wait_cleanup() {
  local id=$1 body=''
  for _ in $(seq 1 200); do
    body=$(curl -fsS "$base/jobs/$id")
    if printf '%s' "$body" | grep -q '"cleanup_status_recorded"'; then
      printf '%s' "$body"
      return 0
    fi
    read -r -t 0.05 <> <(:) || true
  done
  echo "job $id did not finish cleanup: $body" >&2
  return 1
}

wait_log_marker() {
  local file=$1 marker=$2
  for _ in $(seq 1 200); do
    if grep -Fq "$marker" "$file"; then return 0; fi
    read -r -t 0.05 <> <(:) || true
  done
  echo "log marker did not arrive: $marker" >&2
  return 1
}

success_id=$(create_job success)
success_body=$(wait_cleanup "$success_id")
printf '%s' "$success_body" | grep -q '"state":"succeeded"' || {
  echo "expected success Job to succeed: $success_body"
  exit 1
}
printf '%s' "$success_body" | grep -q '"error":null' || {
  echo "success Job reported an error: $success_body"
  exit 1
}
grep -q 'SITOS_WORKER_PARAMETER_OK input=spike-input output=worker:spike-input' "$log"
grep -q 'SITOS_READER_OK .*durable-value' "$log"
"$python" "$reader" --prefix "$prefix" --sid "$success_id" --key worker/durable \
  --zenoh-config "$zenoh_config" --expect-missing

failed_id=$(create_job failed)
failed_body=$(wait_cleanup "$failed_id")
printf '%s' "$failed_body" | grep -q '"state":"failed"' || {
  echo "expected non-zero worker to fail: $failed_body"
  exit 1
}
printf '%s' "$failed_body" | grep -q '"exit_code":3' || {
  echo "expected worker exit code 3: $failed_body"
  exit 1
}
grep -q 'SITOS_WORKER_SCENARIO_OK exit_code=3' "$log" || {
  echo "failed worker scenario did not reach its completion marker"
  exit 1
}
"$python" "$reader" --prefix "$prefix" --sid "$failed_id" --key worker/durable \
  --zenoh-config "$zenoh_config" --expect-missing

cancel_id=$(create_job cancel)
cancel_body=''
for _ in $(seq 1 200); do
  cancel_body=$(curl -fsS "$base/jobs/$cancel_id")
  if printf '%s' "$cancel_body" | grep -q '"worker_running"'; then break; fi
  read -r -t 0.05 <> <(:) || true
done
printf '%s' "$cancel_body" | grep -q '"worker_running"' || {
  echo "cancel Job did not reach worker_running: $cancel_body"
  exit 1
}
cancel_marker="SITOS_READER_OK ${prefix}/buffers/${cancel_id}/durable/worker/durable durable-value"
wait_log_marker "$log" "$cancel_marker"
curl -fsS -X POST "$base/v1/jobs/$cancel_id/cancel" >/dev/null
cancel_body=$(wait_cleanup "$cancel_id")
printf '%s' "$cancel_body" | grep -q '"state":"cancelled"' || {
  echo "expected cancel outcome: $cancel_body"
  exit 1
}
"$python" "$reader" --prefix "$prefix" --sid "$cancel_id" --key worker/durable \
  --zenoh-config "$zenoh_config" --expect-missing
[[ "$(grep -c '^SITOS_SESSION_CLOSED ' "$log")" -eq 3 ]]

publisher_markers=$(grep -E '^SITOS_BUFFER_PUBLISHER_(LIMITATION|OK)' "$log" || true)
[[ "$(printf '%s\n' "$publisher_markers" | sed '/^$/d' | wc -l)" -eq 3 ]] || {
  echo "expected one publisher marker per normal Job: $publisher_markers"
  exit 1
}
while IFS= read -r marker; do
  case "$marker" in
    SITOS_BUFFER_PUBLISHER_OK) ;;
    "SITOS_BUFFER_PUBLISHER_LIMITATION ValueError: Transport does not support Fence") ;;
    *) echo "unexpected publisher marker: $marker"; exit 1 ;;
  esac
done <<< "$publisher_markers"

kill -TERM "$daemon"
wait "$daemon"
daemon=''

failure_scratch="$scratch/create-session-failure"
failure_journal="$failure_scratch/journal"
failure_log="$failure_scratch/daemon.log"
failure_child_marker="$failure_scratch/child-ran"
failure_application="touch \"$failure_child_marker\"; $worker_command"
rm -rf -- "$failure_scratch"
mkdir -p -- "$failure_scratch"
SITOMETRON_SPIKE_FAIL_CREATE_SESSION=1 "$binary" --listen 127.0.0.1:0 \
  --journal "$failure_journal" --workdir "$failure_scratch" \
  --application "create-failure=$failure_application" >"$failure_log" 2>&1 &
daemon=$!
port=''
for _ in $(seq 1 100); do
  port=$(sed -n 's/.*listening on http:\/\/127\.0\.0\.1:\([0-9]*\).*/\1/p' "$failure_log" | head -n1)
  [[ -n "$port" ]] && break
  read -r -t 0.1 <> <(:) || true
done
[[ -n "$port" ]] || { echo "failure daemon did not report a port"; cat "$failure_log"; exit 1; }
base="http://127.0.0.1:$port"
curl -fsS "$base/v1/health" | grep -q '"status":"ok"'
failure_id=$(create_job create-failure)
failure_body=$(wait_cleanup "$failure_id")
printf '%s' "$failure_body" | grep -q '"state":"failed"' || {
  echo "expected CreateSession failure Job to fail: $failure_body"
  exit 1
}
printf '%s' "$failure_body" | grep -q 'Sitos session creation failed:' || {
  echo "CreateSession failure reason was not recorded: $failure_body"
  exit 1
}
[[ ! -e "$failure_child_marker" ]] || {
  echo "CreateSession failure launched a child"
  exit 1
}
! grep -q 'SITOS_WORKER_' "$failure_log" || {
  echo "CreateSession failure worker unexpectedly ran"
  exit 1
}
! grep -q "SITOS_SESSION_CLOSED $failure_id" "$failure_log" || {
  echo "CreateSession failure left a Session to close"
  exit 1
}

kill -TERM "$daemon"
wait "$daemon"
daemon=''
no_node_error="$scratch/no-node-reader.stderr"
if "$python" "$reader" --prefix "$prefix" --sid no-node-session --key worker/durable \
  --zenoh-config "$zenoh_config" --expect-missing >"$scratch/no-node-reader.stdout" \
  2>"$no_node_error"; then
  echo "reader accepted durable absence with no node running"
  cat "$scratch/no-node-reader.stdout" "$no_node_error"
  exit 1
fi
grep -Eq '^SITOS_READER_ERROR .*(control query|Zenoh session failed)' "$no_node_error" || {
  echo "no-node reader failure did not identify the control query"
  cat "$scratch/no-node-reader.stdout" "$no_node_error"
  exit 1
}
grep -q '^SITOS_SESSIONS_OPEN 0$' "$failure_log" || {
  echo "CreateSession failure left an open Session"
  cat "$failure_log"
  exit 1
}
trap - EXIT
echo "sitos smoke ok: success=$success_id failed=$failed_id cancelled=$cancel_id create_failure=$failure_id"
