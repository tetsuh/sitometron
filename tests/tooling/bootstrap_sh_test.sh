#!/usr/bin/env bash
# Deterministic, network-free contract driver for bootstrap.sh.
# This is intentionally a black-box driver: it never adds a production test hook.
set -uo pipefail

readonly TEST_ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/sitometron bootstrap sh test.XXXXXX")"
readonly OFFICIAL_ORIGIN="https://github.com/microsoft/vcpkg.git"
readonly PIN="40f3c709db80acf154ac4b17a1f83c564ebd022e"
readonly -a CHECK_NAMES=(
  bootstrap_missing_host_tool
  bootstrap_invalid_pin
  bootstrap_untrusted_checkout
  bootstrap_stage_failure
  bootstrap_paths_with_spaces
  bootstrap_cold_run
  bootstrap_warm_run_non_destructive
  bootstrap_forbidden_environment
  bootstrap_cache_mismatch
  bootstrap_invalid_existing_executable
  bootstrap_malformed_pin
  bootstrap_lock_contention
)

cleanup() { rm -rf -- "$TEMP_ROOT"; }
trap cleanup EXIT

fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS %s\n' "$*"; }
require() {
  [[ "$1" == "$2" ]] || fail "$3 (actual: $1)"
}

copy_case() {
  local name=$1 case_dir="$TEMP_ROOT/$1" repo="$TEMP_ROOT/$1/repository with spaces"
  mkdir -p -- "$case_dir"
  # cp, rather than git archive, keeps this a realistic repository copy while avoiding network.
  cp -a -- "$TEST_ROOT_DIR/." "$repo"
  rm -rf -- "$repo/build" "$repo/.cache"
  mkdir -p -- "$repo/fake-native-bin"
  printf '%s\n' "$repo"
}
write_pin() {
  local repo=$1 value=${2:-$PIN}
  mkdir -p -- "$repo/tools"
  printf '%s\n' "$value" >"$repo/tools/vcpkg-tool-commit.txt"
}

# Fake tools are deliberately native commands found through PATH.  They only record calls and
# emulate successful process boundaries; all provisioning/configure/build/test failures are
# injected in the fake command selected by FAKE_FAIL_STAGE.
write_fake_tools() {
  local repo=$1 bin="$1/fake-native-bin" log="$1/fake-native.log"
  : >"$log"
  cat >"$bin/fake-event" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${FAKE_LOG:?}"
EOF
  cat >"$bin/git" <<'EOF'
#!/usr/bin/env bash
set -uo pipefail
printf 'git %s\n' "$*" >>"${FAKE_LOG:?}"
[[ ${GIT_TERMINAL_PROMPT:-} == 0 && ${GCM_INTERACTIVE:-} == never ]] || exit 96
if [[ ${1:-} == --version ]]; then printf 'git version 2.43.0\n'; exit 0; fi
if [[ ${FAKE_FAIL_STAGE:-} == clone && ${1:-} == clone ]]; then exit 97; fi
if [[ ${1:-} == clone ]]; then
  dest=${3:?clone destination missing}; mkdir -p -- "$dest/.git"
  mkdir -p -- "$dest/scripts/buildsystems"
  : >"$dest/scripts/buildsystems/vcpkg.cmake"
  cat >"$dest/bootstrap-vcpkg.sh" <<'SCRIPT'
#!/usr/bin/env bash
printf 'bootstrap-vcpkg\n' >>"${FAKE_LOG:?}"
[[ ${FAKE_FAIL_STAGE:-} == bootstrap ]] && exit 97
[[ -d ${VCPKG_DOWNLOADS:?} ]] || exit 98
cat >"$(dirname -- "$0")/vcpkg" <<'VCPKG'
#!/usr/bin/env bash
printf 'vcpkg %s\n' "$*" >>"${FAKE_LOG:?}"
[[ ${FAKE_FAIL_STAGE:-} == install && ${1:-} == install ]] && exit 97
[[ ${1:-} == version ]] && printf 'vcpkg package management program version 2024-01-01\n'
exit 0
VCPKG
chmod +x "$(dirname -- "$0")/vcpkg"
SCRIPT
  chmod +x "$dest/bootstrap-vcpkg.sh"
  exit 0
fi
# The fixture models the read-only Git queries used for trust validation.
case " $* " in
  *' remote get-url origin '*) printf '%s\n' "${FAKE_GIT_ORIGIN:-https://github.com/microsoft/vcpkg.git}";;
  *' rev-parse HEAD '*) printf '%s\n' "${FAKE_GIT_REV:-40f3c709db80acf154ac4b17a1f83c564ebd022e}";;
  *' rev-parse --is-inside-work-tree '*) printf 'true\n';;
  *' status --porcelain --untracked-files=no '*) printf '%s\n' "${FAKE_GIT_STATUS:-}";;
  *) exit 0;;
esac
EOF
  cat >"$bin/cmake" <<'EOF'
#!/usr/bin/env bash
set -uo pipefail
printf 'cmake %s\n' "$*" >>"${FAKE_LOG:?}"
if [[ ${1:-} == --version ]]; then printf 'cmake version 3.28.1\n'; exit 0; fi
if [[ ${FAKE_FAIL_STAGE:-} == configure && ${1:-} == --preset ]]; then exit 97; fi
if [[ ${FAKE_FAIL_STAGE:-} == build && ${1:-} == --build ]]; then exit 97; fi
if [[ ${1:-} == --preset ]]; then
  mkdir -p -- "${FAKE_REPO_ROOT:?}/build/dev-linux"
  : >"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt"
  for arg in "$@"; do case "$arg" in
    -DCMAKE_C_COMPILER=*) printf 'CMAKE_C_COMPILER:FILEPATH=%s\n' "${arg#*=}" >>"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt";;
    -DCMAKE_CXX_COMPILER=*) printf 'CMAKE_CXX_COMPILER:FILEPATH=%s\n' "${arg#*=}" >>"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt";;
    -DCMAKE_TOOLCHAIN_FILE=*) printf 'CMAKE_TOOLCHAIN_FILE:FILEPATH=%s\n' "${arg#*=}" >>"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt";;
    -DVCPKG_TARGET_TRIPLET=*) printf 'VCPKG_TARGET_TRIPLET:STRING=%s\n' "${arg#*=}" >>"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt";;
    -DVCPKG_INSTALLED_DIR=*) printf 'VCPKG_INSTALLED_DIR:PATH=%s\n' "${arg#*=}" >>"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt";;
    -DVCPKG_MANIFEST_INSTALL=*) printf 'VCPKG_MANIFEST_INSTALL:BOOL=%s\n' "${arg#*=}" >>"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt";;
    -DVCPKG_APPLOCAL_DEPS=*) printf 'VCPKG_APPLOCAL_DEPS:BOOL=%s\n' "${arg#*=}" >>"${FAKE_REPO_ROOT:?}/build/dev-linux/CMakeCache.txt";;
  esac; done
fi
exit 0
EOF
  cat >"$bin/ninja" <<'EOF'
#!/usr/bin/env bash
printf 'ninja %s\n' "$*" >>"${FAKE_LOG:?}"
[[ ${1:-} == --version ]] && printf '1.11.1\n'
EOF
  cat >"$bin/ctest" <<'EOF'
#!/usr/bin/env bash
set -uo pipefail
printf 'ctest %s\n' "$*" >>"${FAKE_LOG:?}"
[[ ${1:-} == --version ]] && { printf 'ctest version 3.28.1\n'; exit 0; }
[[ ${FAKE_FAIL_STAGE:-} == test && ${1:-} == --preset ]] && exit 97
exit 0
EOF
  for dependency in curl zip unzip tar; do
    cat >"$bin/$dependency" <<EOF
#!/usr/bin/env bash
printf '$dependency %s\\n' "\$*" >>"\${FAKE_LOG:?}"
[[ \${1:-} == --version || \${1:-} == -v || \${1:-} == --help ]] && printf '$dependency fixture 1.0\\n'
exit 0
EOF
  done
  chmod +x "$bin"/*
}

invoke_wrapper() {
  local repo=$1; shift
  # bash is invoked by absolute path so a deliberately sparse PATH can model a missing tool.
  env "$@" FAKE_LOG="$repo/fake-native.log" FAKE_REPO_ROOT="$repo" PATH="$repo/fake-native-bin:/usr/bin:/bin" \
    bash "$repo/bootstrap.sh"
}
expect_reject() {
  local name=$1 repo=$2; shift 2
  local output status=0
  output=$(invoke_wrapper "$repo" "$@" 2>&1) || status=$?
  (( status != 0 )) || fail "$name unexpectedly passed"
  printf '%s\n' "$output" | grep -Eiq 'fail|error|reject|invalid|lock|stage|tool|checkout|pin' \
    || fail "$name produced no actionable diagnostic"
}

bootstrap_missing_host_tool() {
  local repo output status tool
  for tool in cmake ctest ninja gcc g++; do
    repo=$(copy_case "missing-host-tool-$tool")
    write_fake_tools "$repo"
    # Keep normal shell utilities available but make one required host tool unusable.
    cat >"$repo/fake-native-bin/$tool" <<EOF
#!/usr/bin/env bash
printf '$tool unavailable\\n' >&2
exit 127
EOF
    chmod +x "$repo/fake-native-bin/$tool"
    status=0
    output=$(env FAKE_LOG="$repo/fake-native.log" PATH="$repo/fake-native-bin:/usr/bin:/bin" \
      bash "$repo/bootstrap.sh" 2>&1) || status=$?
    (( status != 0 )) || fail "bootstrap_missing_host_tool/$tool unexpectedly passed"
    grep -Eiq "tool|command|$tool|missing|available|failed" <<<"$output" || fail "missing $tool diagnostic is not actionable"
    ! grep -Fq 'INFO: stage: pin validation' <<<"$output" || fail "unusable $tool reached pin validation"
    ! grep -Eq '^git clone |^bootstrap-vcpkg|^vcpkg install|^cmake --preset|^cmake --build|^ctest --preset' "$repo/fake-native.log" \
      || fail "unusable $tool reached checkout or build work"
  done
  pass bootstrap_missing_host_tool
}

bootstrap_invalid_pin() {
  local repo output status=0
  repo=$(copy_case invalid-pin); write_pin "$repo" invalid; write_fake_tools "$repo"
  expect_reject bootstrap_invalid_pin "$repo"
  ! grep -Eq '^git |^bootstrap-vcpkg|^vcpkg |^cmake --preset|^cmake --build|^ctest --preset' "$repo/fake-native.log" \
    || fail 'invalid pin reached checkout or build work'

  repo=$(copy_case invalid-pin-unsupported-cmake); write_pin "$repo" invalid; write_fake_tools "$repo"
  cat >"$repo/fake-native-bin/cmake" <<'EOF'
#!/usr/bin/env bash
printf 'cmake %s\n' "$*" >>"${FAKE_LOG:?}"
[[ ${1:-} == --version ]] && { printf 'cmake version 3.27.9\n'; exit 0; }
exit 0
EOF
  chmod +x "$repo/fake-native-bin/cmake"
  output=$(invoke_wrapper "$repo" 2>&1) || status=$?
  (( status != 0 )) || fail 'unsupported CMake with an invalid pin unexpectedly passed'
  grep -Fq 'CMake 3.28+ is required' <<<"$output" || fail 'unsupported CMake did not win before pin validation'
  ! grep -Fq 'INFO: stage: pin validation' <<<"$output" || fail 'unsupported CMake reached pin validation'
  ! grep -Eq '^git |^bootstrap-vcpkg|^vcpkg |^cmake --preset|^cmake --build|^ctest --preset' "$repo/fake-native.log" \
    || fail 'unsupported CMake reached checkout or build work'
  pass bootstrap_invalid_pin
}

bootstrap_untrusted_checkout() {
  local variant repo checkout
  for variant in wrong-origin wrong-revision tracked-dirtiness unmanaged-path gitfile ancestor-redirection; do
    repo=$(copy_case "untrusted-$variant"); write_fake_tools "$repo"
    checkout="$repo/.cache/vcpkg/x64-linux"
    mkdir -p -- "$checkout/.git"
    case $variant in
      wrong-origin) export FAKE_GIT_ORIGIN='https://example.invalid/untrusted.git';;
      wrong-revision) export FAKE_GIT_REV='0000000000000000000000000000000000000000';;
      tracked-dirtiness) export FAKE_GIT_STATUS=' M ports/example';;
      unmanaged-path) rm -rf -- "$checkout/.git"; printf x >"$checkout/unmanaged";;
      gitfile) rm -rf -- "$checkout/.git"; printf 'gitdir: %s\n' "$repo/../outside-git-dir" >"$checkout/.git";;
      ancestor-redirection) outside="$repo/../outside-$variant"; mkdir -p -- "$outside"; rm -rf -- "$repo/.cache/vcpkg"; ln -s -- "$outside" "$repo/.cache/vcpkg";;
    esac
    expect_reject "bootstrap_untrusted_checkout/$variant" "$repo"
    if [[ "$variant" == ancestor-redirection ]]; then
      [[ -L "$repo/.cache/vcpkg" ]] || fail "untrusted checkout/$variant was destructively removed"
    else
      [[ -e "$checkout" ]] || fail "untrusted checkout/$variant was destructively removed"
    fi
    unset FAKE_GIT_ORIGIN FAKE_GIT_REV FAKE_GIT_STATUS
  done
  pass bootstrap_untrusted_checkout
}

bootstrap_stage_failure() {
  local stage repo output status
  for stage in clone bootstrap install configure build test; do
    repo=$(copy_case "stage-$stage"); write_fake_tools "$repo"
    status=0; output=$(invoke_wrapper "$repo" FAKE_FAIL_STAGE="$stage" 2>&1) || status=$?
    (( status != 0 )) || fail "bootstrap_stage_failure/$stage unexpectedly passed"
    grep -Eiq "${stage}|fail|error" <<<"$output" || fail "stage failure/$stage lacks stage diagnostic"
    case $stage in
      clone) ! grep -q 'bootstrap-vcpkg' "$repo/fake-native.log" || fail 'clone failure reached bootstrap';;
      bootstrap) ! grep -q '^vcpkg ' "$repo/fake-native.log" || fail 'bootstrap failure reached install';;
      install)
        (( status == 97 )) || fail "install failure returned $status instead of 97"
        grep -q '^vcpkg install ' "$repo/fake-native.log" || fail 'install failure did not reach vcpkg install'
        grep -Fxq 'ERROR [stage manifest provisioning]: command failed with exit 97; retry: ./bootstrap.sh' <<<"$output" \
          || fail 'install failure lacked the exact manifest-provisioning diagnostic'
        ! grep -Eq '^cmake --preset|^cmake --build|^ctest --preset' "$repo/fake-native.log" || fail 'install failure reached configure, build, or test';;
      configure) ! grep -q '^cmake --build' "$repo/fake-native.log" || fail 'configure failure reached build';;
      build) ! grep -q '^ctest --preset' "$repo/fake-native.log" || fail 'build failure reached test';;
    esac
  done
  pass bootstrap_stage_failure
}

bootstrap_paths_with_spaces() {
  local repo; repo=$(copy_case paths-with-spaces); write_fake_tools "$repo"
  invoke_wrapper "$repo" || fail 'paths_with_spaces did not complete with fixtures'
  grep -Fq "$repo" "$repo/fake-native.log" 2>/dev/null || true
  pass bootstrap_paths_with_spaces
}

bootstrap_cold_run() {
  local repo; repo=$(copy_case cold-run); write_fake_tools "$repo"
  invoke_wrapper "$repo" >/dev/null 2>&1 || fail 'cold run did not complete with fixtures'
  grep -q '^git clone ' "$repo/fake-native.log" || fail 'cold run did not clone'
  grep -q '^bootstrap-vcpkg' "$repo/fake-native.log" || fail 'cold run did not bootstrap'
  grep -q '^vcpkg install ' "$repo/fake-native.log" || fail 'cold run did not provision'
  grep -q '^cmake --preset ' "$repo/fake-native.log" || fail 'cold run did not configure'
  grep -q '^cmake --build ' "$repo/fake-native.log" || fail 'cold run did not build'
  grep -q '^ctest --preset' "$repo/fake-native.log" || fail 'cold run did not test'
  pass bootstrap_cold_run
}

bootstrap_warm_run_non_destructive() {
  local repo before after
  repo=$(copy_case warm-run); write_fake_tools "$repo"
  printf 'sentinel\n' >"$repo/warm-sentinel"
  invoke_wrapper "$repo" >/dev/null 2>&1 || fail 'warm setup run failed'
  before_clone=$(grep -c '^git clone ' "$repo/fake-native.log" || true)
  before_bootstrap=$(grep -c '^bootstrap-vcpkg' "$repo/fake-native.log" || true)
  before_install=$(grep -c '^vcpkg install' "$repo/fake-native.log" || true)
  before_configure=$(grep -c '^cmake --preset' "$repo/fake-native.log" || true)
  before_build=$(grep -c '^cmake --build' "$repo/fake-native.log" || true)
  before_test=$(grep -c '^ctest --preset' "$repo/fake-native.log" || true)
  invoke_wrapper "$repo" >/dev/null 2>&1 || fail 'warm rerun failed'
  require "$before_clone" "$(grep -c '^git clone ' "$repo/fake-native.log" || true)" 'warm run cloned again'
  require "$before_bootstrap" "$(grep -c '^bootstrap-vcpkg' "$repo/fake-native.log" || true)" 'warm run bootstrapped again'
  require "$((before_install + 1))" "$(grep -c '^vcpkg install' "$repo/fake-native.log" || true)" 'warm run omitted provisioning'
  require "$((before_configure + 1))" "$(grep -c '^cmake --preset' "$repo/fake-native.log" || true)" 'warm run omitted configure'
  require "$((before_build + 1))" "$(grep -c '^cmake --build' "$repo/fake-native.log" || true)" 'warm run omitted build'
  require "$((before_test + 1))" "$(grep -c '^ctest --preset' "$repo/fake-native.log" || true)" 'warm run omitted test'
  ! grep -Eq ' fetch | reset | clean | checkout --force| rm ' "$repo/fake-native.log" || fail 'warm run invoked destructive operation'
  [[ -f "$repo/warm-sentinel" ]] || fail 'warm run removed sentinel'
  pass bootstrap_warm_run_non_destructive
}

bootstrap_forbidden_environment() {
  local repo; repo=$(copy_case forbidden-environment); write_fake_tools "$repo"
  expect_reject bootstrap_forbidden_environment "$repo" VCPKG_DEFAULT_TRIPLET=bad
  ! grep -Eq '^git |^bootstrap-vcpkg|^vcpkg |^cmake --preset|^cmake --build|^ctest --preset' "$repo/fake-native.log" \
    || fail 'forbidden environment reached checkout or build work'
  pass bootstrap_forbidden_environment
}

bootstrap_cache_mismatch() {
  local repo cache outside before
  repo=$(copy_case cache-mismatch); write_fake_tools "$repo"
  cache="$repo/build/dev-linux/CMakeCache.txt"; mkdir -p "$(dirname "$cache")"
  printf 'CMAKE_C_COMPILER:FILEPATH=/wrong/compiler\n' >"$cache"
  expect_reject bootstrap_cache_mismatch "$repo"
  ! grep -q '^cmake --preset' "$repo/fake-native.log" || fail 'cache mismatch reached configure'
  [[ -f "$cache" ]] || fail 'cache mismatch was destructively removed'

  repo=$(copy_case cache-redirection); write_fake_tools "$repo"
  cache="$repo/build/dev-linux/CMakeCache.txt"; outside="$repo/../outside-cache"
  mkdir -p -- "$(dirname -- "$cache")"; printf 'sentinel\n' >"$outside"; before=$(cat -- "$outside")
  ln -s -- "$outside" "$cache"
  expect_reject bootstrap_cache_redirection "$repo"
  [[ -L "$cache" && "$(cat -- "$outside")" == "$before" ]] || fail 'cache redirection was followed or mutated'
  ! grep -q '^cmake --preset' "$repo/fake-native.log" || fail 'cache redirection reached configure'
  pass bootstrap_cache_mismatch
}

bootstrap_invalid_existing_executable() {
  local repo checkout; repo=$(copy_case invalid-existing-executable); write_fake_tools "$repo"
  checkout="$repo/.cache/vcpkg/x64-linux"; mkdir -p "$checkout/scripts/buildsystems"; : >"$checkout/scripts/buildsystems/vcpkg.cmake"
  mkdir -p "$checkout/.git"; printf 'not executable\n' >"$checkout/vcpkg"
  chmod +x "$checkout/vcpkg"
  expect_reject bootstrap_invalid_existing_executable "$repo"
  repo=$(copy_case zero-output-existing-executable); write_fake_tools "$repo"
  checkout="$repo/.cache/vcpkg/x64-linux"; mkdir -p "$checkout/scripts/buildsystems"; : >"$checkout/scripts/buildsystems/vcpkg.cmake"
  mkdir -p "$checkout/.git"; printf '#!/usr/bin/env bash\nexit 0\n' >"$checkout/vcpkg"; chmod +x "$checkout/vcpkg"
  expect_reject bootstrap_zero_output_existing_executable "$repo"

  repo=$(copy_case non-executable-existing-vcpkg); write_fake_tools "$repo"
  checkout="$repo/.cache/vcpkg/x64-linux"; mkdir -p "$checkout/scripts/buildsystems"; : >"$checkout/scripts/buildsystems/vcpkg.cmake"
  mkdir -p "$checkout/.git"; printf 'existing sentinel\n' >"$checkout/vcpkg"; chmod 600 "$checkout/vcpkg"
  cat >"$checkout/bootstrap-vcpkg.sh" <<'EOF'
#!/usr/bin/env bash
printf 'bootstrap-vcpkg\n' >>"${FAKE_LOG:?}"
cat >"$(dirname -- "$0")/vcpkg" <<'VCPKG'
#!/usr/bin/env bash
[[ ${1:-} == version ]] && printf 'vcpkg package management program version fixture\n'
exit 0
VCPKG
chmod +x "$(dirname -- "$0")/vcpkg"
EOF
  chmod +x "$checkout/bootstrap-vcpkg.sh"
  expect_reject bootstrap_non_executable_existing_vcpkg "$repo"
  [[ $(cat -- "$checkout/vcpkg") == 'existing sentinel' ]] || fail 'non-executable existing vcpkg was overwritten'
  ! grep -q '^bootstrap-vcpkg$' "$repo/fake-native.log" || fail 'non-executable existing vcpkg reached bootstrap'
  pass bootstrap_invalid_existing_executable
}

bootstrap_malformed_pin() {
  local repo; repo=$(copy_case malformed-pin); write_fake_tools "$repo"
  printf '%s\0\n' "$PIN" >"$repo/tools/vcpkg-tool-commit.txt"
  expect_reject bootstrap_malformed_pin "$repo"
  ! grep -Eq '^git |^bootstrap-vcpkg|^vcpkg |^cmake --preset|^cmake --build|^ctest --preset' "$repo/fake-native.log" \
    || fail 'malformed pin reached checkout or build work'
  pass bootstrap_malformed_pin
}

bootstrap_lock_contention() {
  local repo; repo=$(copy_case lock-contention); write_fake_tools "$repo"
  mkdir -p -- "$repo/build/bootstrap-locks"
  printf '%s\n' "pid=$$" >"$repo/build/bootstrap-locks/x64-linux.lock"
  expect_reject bootstrap_lock_contention "$repo"
  [[ -f "$repo/build/bootstrap-locks/x64-linux.lock" ]] || fail 'lock contention removed lock'
  pass bootstrap_lock_contention
}

# Keep this preflight first and fail-stop.  It is the intentional RED assertion: before any
# production implementation, the only failure must be that the canonical root wrapper is absent.
bootstrap_missing_canonical_wrapper() {
  local expected="$TEST_ROOT_DIR/bootstrap.sh"
  [[ -f "$expected" && -x "$expected" ]] || fail "bootstrap_missing_canonical_wrapper: canonical root wrapper is absent at $expected"
  pass bootstrap_missing_canonical_wrapper
}

bootstrap_missing_canonical_wrapper
for check in "${CHECK_NAMES[@]}"; do "$check"; done
printf 'All bootstrap.sh contract checks passed (%d checks).\n' "$(( ${#CHECK_NAMES[@]} + 1 ))"
