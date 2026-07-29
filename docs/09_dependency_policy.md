# Dependency policy

## C++ dependencies

Sitometron uses vcpkg manifest mode for third-party C++ dependencies. `vcpkg.json` pins builtin
baseline `40f3c709db80acf154ac4b17a1f83c564ebd022e`; CI provisioning uses a separately pinned tool
commit when dependencies are introduced. Developers and CI pre-provision `VCPKG_ROOT`; project
CMake never clones, bootstraps, updates, or invokes a package manager.

Conan and general dependency acquisition through CMake `FetchContent` are not used. Runtime code
does not download dependencies. Dependency updates use dedicated pull requests with Linux and
Windows evidence.

`sitometron_core` depends only on the C++ standard library. Adapter targets own all third-party and
platform dependencies.

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
