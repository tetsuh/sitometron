# Issue #17 dependency closure and attribution evidence

This document records the reproducibility inputs and the expected resolved closure for the two
supported triplets. It is audit evidence, not an unofficial lock file. The resolution authority is
the manifest SHA-256, immutable builtin baseline, and independently verified vcpkg tool checkout;
installed trees, downloads, caches, and build artifacts are not committed.

## Resolution inputs

| Input | Linux | Windows |
|---|---|---|
| Manifest SHA-256 | `08970af2a59151c3c4c292b0e7f4b401f290d9a9e9d55b3760ce9f8247084ac8` | same manifest |
| vcpkg builtin baseline | `40f3c709db80acf154ac4b17a1f83c564ebd022e` | same |
| vcpkg tool checkout | `40f3c709db80acf154ac4b17a1f83c564ebd022e` (checked independently) | same |
| Runner / architecture | `ubuntu-24.04` / x64 | `windows-latest` / x64 |
| Triplet | `x64-linux` | `x64-windows` |
| Provisioning command | `vcpkg install --triplet=x64-linux --x-manifest-root=$GITHUB_WORKSPACE --x-install-root=$GITHUB_WORKSPACE/build/vcpkg-installed` | `vcpkg install --triplet=x64-windows --x-manifest-root=$GITHUB_WORKSPACE --x-install-root=$GITHUB_WORKSPACE/build/vcpkg-installed` |
| Configure boundary | pre-provisioned tree, `VCPKG_MANIFEST_INSTALL=OFF` | pre-provisioned tree, `VCPKG_MANIFEST_INSTALL=OFF` |

CI records runner image identity, compiler, CMake, Ninja, and vcpkg versions in each job log. The
filesystem binary cache follows Sitos ADR-0031 and is not required for correctness.

## Resolved packages

The pinned baseline resolves this closure for both triplets. Port revisions are `0` unless shown;
version-date entries are identified explicitly. `vcpkg list` in each CI job is the final installed
closure evidence, including host tools.

| Port | Version / revision | Role | License |
|---|---:|---|---|
| nlohmann-json | 3.12.0 / 2 | direct target | MIT |
| boost-uuid | 1.91.0 / 0 | direct target | BSL-1.0 |
| boost-hash2 | 1.91.0 / 0 | direct target | BSL-1.0 |
| boost-assert | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-cmake | 1.91.0 / 0 | transitive build support | BSL-1.0 |
| boost-config | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-headers | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-throw-exception | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-type-traits | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-container-hash | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-describe | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-mp11 | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-uninstall | 1.91.0 / 0 | transitive build support | MIT |
| vcpkg-boost | 2025-03-29 / 0 | transitive build support | MIT |
| vcpkg-cmake | 2024-04-23 / 0 | host tool | MIT |
| vcpkg-cmake-config | 2026-07-21 / 0 | host tool | MIT |

The Boost transitive ports are opaque prerequisites: Sitometron source does not include or link
them directly. The header-only selected targets require no additional project-owned system link
requirement on either supported triplet; CI's generated link and package metadata remain the final
platform evidence.

Resolved edges are: `nlohmann-json -> vcpkg-cmake (host), vcpkg-cmake-config (host)`;
`boost-uuid -> boost-assert, boost-cmake, boost-config, boost-headers, boost-throw-exception,
boost-type-traits`; `boost-hash2 -> boost-assert, boost-cmake, boost-config, boost-container-hash,
boost-describe, boost-headers, boost-mp11`; `boost-cmake -> boost-uninstall, vcpkg-boost`;
`vcpkg-boost -> vcpkg-cmake (host), vcpkg-cmake-config (host)`; and the shared Boost edges shown
above. The baseline's version constraints select these entries.

## Attribution

The direct `nlohmann-json` port and its source distribution require MIT attribution. Boost.UUID,
Boost.Hash2, and the Boost closure require the Boost Software License 1.0 terms. Attribution text is
sourced from the upstream distribution files (`LICENSE`, `LICENSE_1_0.txt`) and the vcpkg port
metadata at the pinned baseline. Distribution must retain those notices and license text. This
project does not add a project-level `NOTICE` file.
