# Dependency policy

## C++ dependencies

Sitometron uses vcpkg manifest mode for third-party C++ dependencies. The repository pins the
builtin baseline and CI provisioning commit. Developers and CI pre-provision `VCPKG_ROOT`; project
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

Python dependencies use `pyproject.toml` plus a committed lock file. They are not managed by vcpkg
or Conan.

## CI and caches

Binary caches are performance optimizations and never correctness authorities. A clean build must
remain reproducible from pinned manifests and documented provisioning inputs.
