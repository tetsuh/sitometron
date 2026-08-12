# Dependency policy

## C++ dependencies

Sitometron uses vcpkg manifest mode for third-party C++ dependencies. `vcpkg.json` pins builtin
baseline `40f3c709db80acf154ac4b17a1f83c564ebd022e`; the repository-owned tool pin independently
authorizes the vcpkg tool checkout. The canonical wrappers own `VCPKG_ROOT`, `VCPKG_DOWNLOADS`,
and `VCPKG_DISABLE_METRICS` for each invocation. Project CMake never clones, bootstraps, updates,
or invokes a package manager.

Conan and general dependency acquisition through CMake `FetchContent` are not used. Runtime code
does not download dependencies. Dependency updates use dedicated pull requests with Linux and
Windows evidence.

Accepted [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) makes `sitometron_core`
dependency-minimal. It preserves the reviewed standard-header allowlist and permits only the direct
manifest ports and CMake targets for `nlohmann-json`, `boost-uuid`, and `boost-hash2`.
Dependency-owned types remain out of public core headers, and baseline-resolved transitive packages
are opaque build prerequisites rather than direct source-level authorization. Adapter targets own
all other third-party, platform, I/O, and framework dependencies.

Issue #17 implemented the allowlist and activated all five stable `NFR-005` checks on Linux and
Windows. Accepted ADR-0003 and Issue #12 later authorized only the private synchronization headers
and private `Threads::Threads` linkage required by the single writer, without adding a manifest
dependency or public concurrency type. The exact reviewed standard-header, direct-target/include,
private-link, and public-API rules are encoded in the mechanical dependency checks rather than
repeated as a partial list here. UUID generator headers, unapproved Boost components, transitive
headers, dependency-owned public types, and concurrency-owned public types remain rejected.

Sitos remains outside the Sitometron vcpkg manifest. Phase 4 builds and installs one pinned Sitos
release or commit separately, verifies provenance and required contracts, and consumes it as an
installed CMake package through `find_package`.

## Python dependencies

Python dependencies use `pyproject.toml` plus a committed `uv.lock`. They are not managed by vcpkg
or Conan. CI provisions the exact `uv` and Python versions outside project CMake, then uses
`uv sync --frozen`; dependency resolution or lock-file updates are never implicit build steps.

Phase 0A permits Python only for development-time Draft 2020-12 schema validation. This exception
does not make Python a dependency of `sitometron_core`, `sitometrond`, a Worker, an SDK, or a product
package. Runtime or product-build use requires a separately reviewed decision.

## CI and caches

Issue #25 supplies `./bootstrap.sh` and `.\bootstrap.ps1` as the canonical one-command boundary.
Both read the independent lowercase tool pin from `tools/vcpkg-tool-commit.txt`; this tool-checkout
authority is distinct from `vcpkg.json`'s `builtin-baseline` manifest authority. The wrappers use
only the official vcpkg origin, validate exact HEAD/origin/clean tracked state, provision the root
manifest on every run, and pass the five explicit CMake boundary values: `CMAKE_TOOLCHAIN_FILE`,
`VCPKG_TARGET_TRIPLET`, `VCPKG_INSTALLED_DIR`, `VCPKG_MANIFEST_INSTALL=OFF`, and
`VCPKG_APPLOCAL_DEPS=OFF`. Linux/WSL and native Windows never share checkout, installed, configured,
or lock paths. Existing incompatible artifacts fail closed without repair, and lock contention or
stale locks require manual inspection/remediation.

CI reads and validates the repository-owned tool pin, independently provisions official vcpkg at
that exact revision, and provisions `x64-linux` or `x64-windows` outside project CMake.
Normal configure uses the pre-provisioned tree with `VCPKG_MANIFEST_INSTALL=OFF`. Under Accepted
ADR-0004 and Issue #17, Sitometron independently owns a filesystem binary-cache policy whose key
shape is intentionally compatible with the model documented by Sitos ADR-0031. SHA-pinned
`actions/cache` restore/save steps use a key containing cache format, OS, architecture, triplet,
manifest hash, and runner image identity, with a compatible-prefix restore migration. Cache misses
remain fully correct and the cache is never a resolution authority. The complete closure and license
evidence is maintained in
[`dependency_closure.md`](dependency_closure.md).

Binary caches are performance optimizations and never correctness authorities. A clean build must
remain reproducible from pinned manifests and documented provisioning inputs. Project CMake and
runtime targets never invoke `uv`, Python package installers, or network retrieval. Distribution
retains nlohmann/json MIT attribution and applicable Boost Software License 1.0 terms for the full
resolved closure; no project `NOTICE` file is added.
