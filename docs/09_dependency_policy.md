# Dependency policy

## C++ dependencies

Sitometron uses vcpkg manifest mode for third-party C++ dependencies. `vcpkg.json` pins builtin
baseline `40f3c709db80acf154ac4b17a1f83c564ebd022e`; CI provisioning uses a separately pinned tool
commit when dependencies are introduced. Developers and CI pre-provision `VCPKG_ROOT`; project
CMake never clones, bootstraps, updates, or invokes a package manager.

Conan and general dependency acquisition through CMake `FetchContent` are not used. Runtime code
does not download dependencies. Dependency updates use dedicated pull requests with Linux and
Windows evidence.

Accepted [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) makes `sitometron_core`
dependency-minimal. It preserves the reviewed standard-header allowlist and permits only the direct
manifest ports and CMake targets for `nlohmann-json`, `boost-uuid`, and `boost-hash2`.
Dependency-owned types remain out of public core headers, and baseline-resolved transitive packages
are opaque build prerequisites rather than direct source-level authorization. Adapter targets own
all other third-party, platform, I/O, and framework dependencies.

Issue #17 activates the allowlist. Production core source may include only `<string_view>` and
`<nlohmann/json.hpp>`, `<boost/uuid/uuid.hpp>`, `<boost/uuid/string_generator.hpp>`,
`<boost/uuid/uuid_io.hpp>`, and `<boost/hash2/sha2.hpp>`. UUID generator headers and all
baseline-resolved transitive headers remain rejected. The five stable `NFR-005` checks mechanically
enforce this boundary and reject direct target, private include, and public API leakage.

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

CI independently checks out official vcpkg at `40f3c709db80acf154ac4b17a1f83c564ebd022e`, the same
value used as `builtin-baseline`, and provisions `x64-linux` or `x64-windows` outside project CMake.
Normal configure uses the pre-provisioned tree with `VCPKG_MANIFEST_INSTALL=OFF`. The filesystem
binary cache follows Sitos ADR-0031: its SHA-pinned `actions/cache` restore/save steps use a key
containing cache format, OS, architecture, triplet, manifest hash, and runner image identity, with a
compatible-prefix restore migration. Cache misses remain fully correct and the cache is never a
resolution authority. The complete closure and license evidence is maintained in
[`dependency_closure.md`](dependency_closure.md).

Binary caches are performance optimizations and never correctness authorities. A clean build must
remain reproducible from pinned manifests and documented provisioning inputs. Project CMake and
runtime targets never invoke `uv`, Python package installers, or network retrieval. Distribution
retains nlohmann/json MIT attribution and applicable Boost Software License 1.0 terms for the full
resolved closure; no project `NOTICE` file is added.
