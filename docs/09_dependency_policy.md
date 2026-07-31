# Dependency policy

## C++ dependencies

Sitometron uses vcpkg manifest mode for third-party C++ dependencies. `vcpkg.json` pins builtin
baseline `40f3c709db80acf154ac4b17a1f83c564ebd022e`; CI provisioning uses a separately pinned tool
commit when dependencies are introduced. Developers and CI pre-provision `VCPKG_ROOT`; project
CMake never clones, bootstraps, updates, or invokes a package manager.

Conan and general dependency acquisition through CMake `FetchContent` are not used. Runtime code
does not download dependencies. Dependency updates use dedicated pull requests with Linux and
Windows evidence.

`sitometron_core` currently depends only on the C++ standard library under ADR-0001. Adapter targets
own all other third-party, platform, I/O, and framework dependencies.

> **Proposed, not yet normative:** [ADR-0004](adr/0004-allow-explicit-core-dependencies.md) would
> preserve the reviewed standard-header allowlist and allow only the direct manifest ports and CMake
> targets for `nlohmann-json`, `boost-uuid`, and `boost-hash2`. Dependency-owned types would remain
> out of public core headers. Baseline-resolved transitive packages would be opaque build
> prerequisites, not direct source-level authorization.
> ADR-0001 remains effective until ADR-0004 is owner-accepted, and manifest/CMake integration remains
> blocked on a separate implementation Issue.

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

Binary caches are performance optimizations and never correctness authorities. A clean build must
remain reproducible from pinned manifests and documented provisioning inputs. Project CMake and
runtime targets never invoke `uv`, Python package installers, or network retrieval.
