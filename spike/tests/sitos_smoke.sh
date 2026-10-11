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
daemon=''

command -v curl >/dev/null || { echo "curl is required"; exit 2; }
[[ -x "$python" ]] || { echo "Python interpreter is not executable: $python"; exit 2; }
[[ -f "$worker" && -f "$reader" ]] || { echo "worker or reader script is missing"; exit 2; }

worker_command="exec \"$python\" \"$worker\" --reader \"$reader\""
failed_command="$worker_command --exit-code 3"
cancel_command="$worker_command --sleep-after 30"
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
"$python" "$reader" --prefix "$prefix" --sid "$success_id" --key worker/durable --expect-missing

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
"$python" "$reader" --prefix "$prefix" --sid "$failed_id" --key worker/durable --expect-missing

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
curl -fsS -X POST "$base/v1/jobs/$cancel_id/cancel" >/dev/null
cancel_body=$(wait_cleanup "$cancel_id")
printf '%s' "$cancel_body" | grep -q '"state":"cancelled"' || {
  echo "expected cancel outcome: $cancel_body"
  exit 1
}
"$python" "$reader" --prefix "$prefix" --sid "$cancel_id" --key worker/durable --expect-missing

kill -TERM "$daemon"
wait "$daemon"
daemon=''
trap - EXIT
echo "sitos smoke ok: success=$success_id failed=$failed_id cancelled=$cancel_id"
