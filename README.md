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

Requirements:

- CMake 3.28 or later
- Ninja
- a C++20 compiler
- the official vcpkg checkout at the independently reviewed tool commit used by CI
- the root manifest pre-provisioned outside project CMake for the selected platform

Pending Issue #25's canonical developer-bootstrap task, developers explicitly mirror the
[pinned CI provisioning procedure](.github/workflows/ci.yml). The configure step must receive the
pre-provisioned toolchain, triplet, and installed tree and must disable manifest installation and
app-local deployment from project CMake.

Linux, after provisioning `build/vcpkg-installed` for `x64-linux`:

```sh
cmake --preset dev-linux \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-installed" \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DVCPKG_APPLOCAL_DEPS=OFF
cmake --build --preset dev-linux
ctest --preset dev-linux
```

Windows, after provisioning `build/vcpkg-installed` for `x64-windows`:

```powershell
cmake --preset dev-windows `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg-installed" `
  -DVCPKG_MANIFEST_INSTALL=OFF `
  -DVCPKG_APPLOCAL_DEPS=OFF
cmake --build --preset dev-windows
ctest --preset dev-windows
```

See [the build and test specification](docs/06_build_test_packaging.md) and
[dependency policy](docs/09_dependency_policy.md) for the current acquisition boundary and evidence.
In-source builds are rejected.

## License

Apache License 2.0. See [LICENSE](LICENSE).
