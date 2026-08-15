#!/usr/bin/env bash
# Canonical deterministic developer bootstrap for Linux/WSL.
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT
readonly OFFICIAL_ORIGIN='https://github.com/microsoft/vcpkg.git'
readonly TRIPLET='x64-linux'
readonly PRESET='dev-linux'
readonly CHECKOUT="$ROOT/.cache/vcpkg/$TRIPLET"
readonly INSTALLED="$ROOT/build/vcpkg-installed/$TRIPLET"
readonly BUILD_TREE="$ROOT/build/dev-linux"
readonly LOCK="$ROOT/build/bootstrap-locks/$TRIPLET.lock"
cd -- "$ROOT"
STAGE='initialization'
LOCK_HELD=0
LOCK_TOKEN="$$-${RANDOM}-$(date +%s%N)"
PIN=''
VCPKG=''
TOOLCHAIN=''
CC=''
CXX=''

usage() {
  cat <<'EOF'
Usage: ./bootstrap.sh [--help|-h]

Provision the pinned x64-linux vcpkg tree, configure, build, and run CTest.
EOF
}
die() { printf 'ERROR [%s]: %s\n' "$STAGE" "$*" >&2; exit 1; }
set_stage() {
  STAGE=$1
  printf 'INFO: stage: %s\n' "$STAGE"
  printf 'INFO: paths: checkout=%s installed=%s build=%s lock=%s\n' "$CHECKOUT" "$INSTALLED" "$BUILD_TREE" "$LOCK"
}
cleanup() {
  if (( LOCK_HELD )) && [[ -f "$LOCK" ]] && [[ "$(cat -- "$LOCK" 2>/dev/null || true)" == "$LOCK_TOKEN" ]]; then
    rm -f -- "$LOCK"
  fi
}
managed_path_check() {
  local path=$1 current="$ROOT" component rel
  [[ "$path" == "$ROOT"/* ]] || die "managed path is outside the repository root: $path"
  rel=${path#"$ROOT"/}
  IFS='/' read -r -a components <<< "$rel"
  for component in "${components[@]}"; do
    [[ -n "$component" ]] || continue
    current="$current/$component"
    [[ ! -L "$current" ]] || die "managed path has a symlink/reparse ancestor: $current"
  done
}
failure() {
  local status=$?
  printf 'ERROR [stage %s]: command failed with exit %d; retry: ./bootstrap.sh\n' "$STAGE" "$status" >&2
  exit "$status"
}
trap cleanup EXIT
trap failure ERR

case "$#:$*" in
  0:) ;;
  1:-h|1:--help) usage; exit 0 ;;
  *) die 'only -h/--help is supported; this command has no repair, offline, or alternate-path mode' ;;
esac

set_stage 'host validation'
[[ "$(uname -s)" == Linux ]] || die 'Bash bootstrap supports Linux/WSL only'
for tool in git cmake ninja gcc g++ curl zip unzip tar ctest; do
  command -v "$tool" >/dev/null 2>&1 || die "required host tool is missing: $tool"
done
cmake_version=$(cmake --version | awk 'NR==1 {print $3}')
cmake_major=${cmake_version%%.*}; cmake_rest=${cmake_version#*.}; cmake_minor=${cmake_rest%%.*}
[[ $cmake_major =~ ^[0-9]+$ && $cmake_minor =~ ^[0-9]+$ ]] || die 'unable to determine CMake version'
(( cmake_major > 3 || (cmake_major == 3 && cmake_minor >= 28) )) || die "CMake 3.28+ is required (found $cmake_version)"
printf 'INFO: CMake %s\n' "$cmake_version"
printf 'INFO: Ninja %s\n' "$(ninja --version)"
printf 'INFO: GCC %s\n' "$(gcc --version | head -n1)"
printf 'INFO: G++ %s\n' "$(g++ --version | head -n1)"
ctest_version=$(ctest --version) || die 'required host tool is unusable: ctest --version failed'
printf 'INFO: CTest %s\n' "${ctest_version%%$'\n'*}"
CC=$(command -v gcc); CXX=$(command -v g++)
for name in VCPKG_ROOT VCPKG_OVERLAY_PORTS VCPKG_OVERLAY_TRIPLETS VCPKG_CHAINLOAD_TOOLCHAIN_FILE VCPKG_DEFAULT_TRIPLET VCPKG_DEFAULT_HOST_TRIPLET VCPKG_FEATURE_FLAGS; do
  [[ -z "${!name:-}" ]] || die "selection environment $name is set; unset it before retrying"
done
for name in GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_NAMESPACE; do
  [[ -z "${!name:-}" ]] || die "unsafe Git repository environment $name is set; unset it before retrying"
done
for name in VCPKG_BINARY_SOURCES VCPKG_ASSET_SOURCES HTTPS_PROXY HTTP_PROXY ALL_PROXY GIT_CONFIG_COUNT; do
  if [[ -n "${!name:-}" ]]; then printf 'INFO: optional cache/transport override %s is active (value hidden)\n' "$name"; fi
done

set_stage 'pin validation'
pin_file="$ROOT/tools/vcpkg-tool-commit.txt"
[[ -f "$pin_file" ]] || die "repository-owned pin is missing: $pin_file"
[[ "$(wc -c <"$pin_file")" -eq 41 ]] || die 'tool pin must be exactly 41 bytes: 40 lowercase hex bytes and LF'
[[ "$(tail -c 1 "$pin_file" | od -An -t x1 | tr -d '[:space:]')" == 0a ]] || die 'tool pin must end with a newline'
PIN="$(head -c 40 "$pin_file")"
[[ $PIN =~ ^[0-9a-f]{40}$ ]] || die 'tool pin must be exactly one lowercase full 40-hex commit'

set_stage 'lock acquisition'
for managed in "$CHECKOUT" "$INSTALLED" "$BUILD_TREE" "$LOCK" "$CHECKOUT/downloads" "$CHECKOUT/scripts/buildsystems/vcpkg.cmake"; do
  managed_path_check "$managed"
done
mkdir -p -- "$(dirname -- "$LOCK")"
if (set -o noclobber; printf '%s\n' "$LOCK_TOKEN" >"$LOCK") 2>/dev/null; then
  LOCK_HELD=1
else
  die "exclusive lock exists: $LOCK; inspect that no bootstrap is active, then remove it and retry ./bootstrap.sh"
fi

export VCPKG_ROOT="$CHECKOUT"
export VCPKG_DOWNLOADS="$CHECKOUT/downloads"
export VCPKG_DISABLE_METRICS=1
export GIT_TERMINAL_PROMPT=0
export GCM_INTERACTIVE=never
printf 'INFO: repository root: %s\nINFO: checkout: %s\nINFO: installed tree: %s\nINFO: build tree: %s\nINFO: triplet/preset: %s/%s\nINFO: pinned revision: %s\n' "$ROOT" "$CHECKOUT" "$INSTALLED" "$BUILD_TREE" "$TRIPLET" "$PRESET" "$PIN"
printf 'INFO: network-capable stages may include clone, vcpkg bootstrap, pinned port sources, configured caches, and proxies; official clone origin: %s\n' "$OFFICIAL_ORIGIN"

set_stage 'checkout validation'
if [[ ! -e "$CHECKOUT" ]]; then
  mkdir -p -- "$(dirname -- "$CHECKOUT")"
  git clone "$OFFICIAL_ORIGIN" "$CHECKOUT"
  git -C "$CHECKOUT" checkout --detach "$PIN"
elif [[ ! -d "$CHECKOUT" || ! -d "$CHECKOUT/.git" || -L "$CHECKOUT/.git" ]]; then
  die "existing checkout is not a managed Git checkout: $CHECKOUT; move it manually and retry"
fi
[[ "$(git -C "$CHECKOUT" rev-parse --is-inside-work-tree)" == true ]] || die "invalid Git checkout: $CHECKOUT"
[[ "$(git -C "$CHECKOUT" remote get-url origin)" == "$OFFICIAL_ORIGIN" ]] || die "checkout origin is not the official vcpkg origin: $CHECKOUT"
[[ "$(git -C "$CHECKOUT" rev-parse HEAD)" == "$PIN" ]] || die "checkout HEAD is not the repository-owned pin: $CHECKOUT"
[[ -z "$(git -C "$CHECKOUT" status --porcelain --untracked-files=no)" ]] || die "checkout has tracked/index dirtiness: $CHECKOUT"
TOOLCHAIN="$CHECKOUT/scripts/buildsystems/vcpkg.cmake"
[[ -f "$TOOLCHAIN" && ! -L "$TOOLCHAIN" ]] || die "vcpkg checkout is incomplete (toolchain missing): $TOOLCHAIN"
managed_path_check "$VCPKG_DOWNLOADS"
mkdir -p -- "$VCPKG_DOWNLOADS"

set_stage 'vcpkg bootstrap'
VCPKG="$CHECKOUT/vcpkg"
if [[ ! -x "$VCPKG" ]]; then
  [[ -x "$CHECKOUT/bootstrap-vcpkg.sh" ]] || die "vcpkg bootstrap script is missing: $CHECKOUT/bootstrap-vcpkg.sh"
  (cd "$CHECKOUT" && ./bootstrap-vcpkg.sh -disableMetrics)
fi
managed_path_check "$VCPKG"
[[ -x "$VCPKG" ]] || die "vcpkg bootstrap did not create an executable: $VCPKG"
version_output=$("$VCPKG" version 2>&1) || die "vcpkg executable could not report its version: $VCPKG"
[[ "$version_output" =~ [Vv]cpkg.*[Vv]ersion ]] || die "vcpkg executable returned no recognizable version: $VCPKG"
printf 'INFO: vcpkg qualification: %s\n' "$version_output"

set_stage 'manifest provisioning'
mkdir -p -- "$INSTALLED"
"$VCPKG" install --triplet="$TRIPLET" --x-manifest-root="$ROOT" --x-install-root="$INSTALLED"

cache_value() {
  local key=$1
  grep -E "^${key}(:[^=]*)?=" "$BUILD_TREE/CMakeCache.txt" | head -n1 | sed 's/^[^=]*=//'
}
check_cache() {
  managed_path_check "$BUILD_TREE/CMakeCache.txt"
  [[ -f "$BUILD_TREE/CMakeCache.txt" && ! -L "$BUILD_TREE/CMakeCache.txt" ]] || die "configured cache is missing or not a regular file: $BUILD_TREE/CMakeCache.txt"
  local key expected actual
  for key in CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_TOOLCHAIN_FILE VCPKG_TARGET_TRIPLET VCPKG_INSTALLED_DIR VCPKG_MANIFEST_INSTALL VCPKG_APPLOCAL_DEPS; do
    case "$key" in
      CMAKE_C_COMPILER) expected="$CC";;
      CMAKE_CXX_COMPILER) expected="$CXX";;
      CMAKE_TOOLCHAIN_FILE) expected="$TOOLCHAIN";;
      VCPKG_TARGET_TRIPLET) expected="$TRIPLET";;
      VCPKG_INSTALLED_DIR) expected="$INSTALLED";;
      *) expected=OFF;;
    esac
    actual=$(cache_value "$key" || true)
    [[ -n "$actual" && "$actual" == "$expected" ]] || die "CMake cache identity mismatch for $key (expected $expected)"
  done
}
if [[ -e "$BUILD_TREE/CMakeCache.txt" || -L "$BUILD_TREE/CMakeCache.txt" ]]; then
  set_stage 'pre-configure cache validation'; check_cache
fi

set_stage 'configure'
cmake --preset "$PRESET" \
  "-DCMAKE_C_COMPILER=$CC" "-DCMAKE_CXX_COMPILER=$CXX" \
  "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN" "-DVCPKG_TARGET_TRIPLET=$TRIPLET" \
  "-DVCPKG_INSTALLED_DIR=$INSTALLED" -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_APPLOCAL_DEPS=OFF
set_stage 'post-configure cache validation'; check_cache
set_stage 'build'; cmake --build --preset "$PRESET"
set_stage 'test'; ctest --preset "$PRESET"
printf 'SUCCESS: bootstrap completed for %s at %s\n' "$TRIPLET" "$ROOT"
