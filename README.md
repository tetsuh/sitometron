# sitometron

Sitometron is a cross-platform controller for admitting, scheduling, supervising, and observing
trusted local compute applications. It is designed as an application-agnostic control plane and
keeps computation graphs, parameter interpretation, and artifact payloads outside its core.

The project is in its bootstrap phase. No production API or compatibility guarantee exists yet.

## Design boundaries

`sitometron_core` is a C++20 dependency-minimal domain library under Accepted
[ADR-0004](docs/adr/0004-allow-explicit-core-dependencies.md). Its implemented closed allowlist
permits nlohmann/json, Boost.UUID, and Boost.Hash2 behind Sitometron-owned public types, plus the
private synchronization facilities approved by
[ADR-0003](docs/adr/0003-define-single-state-writer-ingress-contract.md). The five active
`NFR-005` checks reject unapproved headers, targets, includes, private links, and public dependency
or concurrency leakage on Linux and Windows. HTTP, Sitos, Zenoh, Holoscan, Python, logging
backends, hardware-topology libraries, and platform process APIs remain in adapters composed only
by `sitometrond`.

See:

- [Overview](docs/00_overview.md)
- [Requirements](docs/01_requirements.md)
- [Architecture](docs/02_architecture.md)
- [Core contracts](docs/03_core_contracts.md)
- [Build and test](docs/06_build_test_packaging.md)
- [Issue breakdown](docs/07_issue_breakdown.md)
- [Contract Registry](docs/08_contract_registry.md)
- [Dependency policy](docs/09_dependency_policy.md)
- [ADR process](docs/10_adr_process.md)
- [Clean-room policy](docs/clean-room-policy.md)
- [Development workflow](docs/development_workflow.md)

## Build

Supported host prerequisites:

- Linux/WSL: Bash, Git, CMake 3.28+, Ninja, GCC/G++, curl, zip/unzip, and tar.
- Native Windows: PowerShell 7.3+, Git, CMake 3.28+, Ninja, and an initialized x64 MSVC Developer PowerShell with `cl.exe`.

The wrapper owns the pinned vcpkg checkout and manifest provisioning; no pre-provisioned checkout is required. Use the canonical, fail-fast bootstrap from the repository root. It reads the independent lowercase
vcpkg tool pin in `tools/vcpkg-tool-commit.txt`, uses the official
`https://github.com/microsoft/vcpkg.git` origin, provisions outside project CMake, and runs the
matching configure, build, and full CTest presets:

```sh
./bootstrap.sh
```

```powershell
.\bootstrap.ps1
```

Linux uses `.cache/vcpkg/x64-linux`, `build/vcpkg-installed/x64-linux`, `build/dev-linux`, and
`build/bootstrap-locks/x64-linux.lock`. Windows uses `.cache/vcpkg/x64-windows`,
`build/vcpkg-installed/x64-windows`, `build/dev-windows`, and
`build/bootstrap-locks/x64-windows.lock`. First runs may
use the network for clone, vcpkg bootstrap, pinned port sources, caches, or proxies; diagnostics
never print credential-bearing values. Warm runs reprovision explicitly and never repair, reset,
clean, or delete an existing checkout. Native Windows requires PowerShell 7.3+ in an initialized
x64 MSVC Developer PowerShell. See the [build and test specification](docs/06_build_test_packaging.md).

For manual Linux verification after provisioning `build/vcpkg-installed/x64-linux`:

```sh
cmake --preset dev-linux \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-installed/x64-linux" \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DVCPKG_APPLOCAL_DEPS=OFF
cmake --build --preset dev-linux
ctest --preset dev-linux
```

For manual Windows verification after provisioning `build/vcpkg-installed/x64-windows`:

```powershell
cmake --preset dev-windows `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-installed/x64-windows" `
  -DVCPKG_MANIFEST_INSTALL=OFF `
  -DVCPKG_APPLOCAL_DEPS=OFF
cmake --build --preset dev-windows
ctest --preset dev-windows
```

See [the build and test specification](docs/06_build_test_packaging.md) and
[dependency policy](docs/09_dependency_policy.md) for the acquisition boundary, pin distinction,
and CI-equivalent evidence.
In-source builds are rejected.

## License

Apache License 2.0. See [LICENSE](LICENSE).
