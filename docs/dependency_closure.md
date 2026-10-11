# Issue #17 and Issue #96 dependency closure and attribution evidence

This document records the reproducibility inputs and the resolved closure for the two supported
triplets. It is audit evidence, not an unofficial lock file. The resolution authority is the
manifest SHA-256, immutable builtin baseline, and independently verified vcpkg tool checkout;
installed trees, downloads, caches, and build artifacts are not committed.

## Resolution inputs

| Input | Linux | Windows |
|---|---|---|
| Manifest SHA-256 | `0be6fee3d01683db8408ededa7d894f8f39f0f18a7bd6958b71e4b68a836c32e` | same manifest |
| vcpkg builtin baseline | `40f3c709db80acf154ac4b17a1f83c564ebd022e` | same |
| vcpkg tool checkout | repository-owned `tools/vcpkg-tool-commit.txt` pin (`40f3c709db80acf154ac4b17a1f83c564ebd022e`, checked independently) | same independent tool pin |
| Runner / architecture | `ubuntu-24.04` / x64 | `windows-latest` / x64 |
| Triplet | `x64-linux` | `x64-windows` |
| Provisioning command | `vcpkg install --triplet=x64-linux --x-manifest-root=$GITHUB_WORKSPACE --x-install-root=$GITHUB_WORKSPACE/build/vcpkg-installed/x64-linux` | `vcpkg install --triplet=x64-windows --x-manifest-root=$GITHUB_WORKSPACE --x-install-root=$GITHUB_WORKSPACE/build/vcpkg-installed/x64-windows` |
| Configure boundary | pre-provisioned tree, `VCPKG_MANIFEST_INSTALL=OFF` | pre-provisioned tree, `VCPKG_MANIFEST_INSTALL=OFF` |

CI records runner image identity, compiler, CMake, Ninja, vcpkg version, and the complete
`vcpkg list` output in each job. The two feature records shown by `vcpkg list` for
`boost-asio[deadline-timer]` and `boost-asio[spawn]` are features of one port, not additional
ports.

Issue #17 resolved 16 ports. Issue #96 resolves 63 ports for each supported triplet, confirming
ADR-0010 Section 7's count of 47 added ports. The Linux and Windows CI lists are the platform
closure evidence; the Windows run confirms the previously inferred desktop closure.

## Resolved packages

Port revisions are `0` unless shown. Version-date entries are identified explicitly. Roles describe
Sitometron's use of the resolved package; transitive packages remain opaque prerequisites.

| Port | Version / revision | Role | License |
|---|---:|---|---|
| boost-algorithm | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-align | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-array | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-asio | 1.91.0 / 0 | direct target; `core`, `deadline-timer`, `spawn` features | BSL-1.0 |
| boost-assert | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-beast | 1.91.0 / 0 | direct target | BSL-1.0 |
| boost-bind | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-cmake | 1.91.0 / 0 | transitive build support | BSL-1.0 |
| boost-compat | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-concept-check | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-config | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-container-hash | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-container | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-context | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-conversion | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-core | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-date-time | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-describe | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-detail | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-endian | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-exception | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-function-types | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-function | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-functional | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-fusion | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-hash2 | 1.91.0 / 0 | direct target | BSL-1.0 |
| boost-headers | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-integer | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-intrusive | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-io | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-iterator | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-lexical-cast | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-logic | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-move | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-mp11 | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-mpl | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-numeric-conversion | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-optional | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-pool | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-predef | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-preprocessor | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-range | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-regex | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-smart-ptr | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-static-assert | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-static-string | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-system | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-throw-exception | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-tokenizer | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-tuple | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-type-index | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-type-traits | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-typeof | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-uninstall | 1.91.0 / 0 | transitive build support | MIT |
| boost-unordered | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-utility | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-uuid | 1.91.0 / 0 | direct target | BSL-1.0 |
| boost-variant2 | 1.91.0 / 0 | transitive | BSL-1.0 |
| boost-winapi | 1.91.0 / 0 | transitive | BSL-1.0 |
| nlohmann-json | 3.12.0 / 2 | direct target | MIT |
| vcpkg-boost | 2025-03-29 / 0 | transitive build support | MIT |
| vcpkg-cmake-config | 2026-07-21 / 0 | host tool | MIT |
| vcpkg-cmake | 2024-04-23 / 0 | host tool | MIT |

The Boost transitive ports are opaque prerequisites: Sitometron source does not include or link
them directly. `boost-context`, `boost-date-time`, and `boost-container` are resolved by the
approved Boost targets but are not direct Sitometron dependencies. The selected targets require no
additional project-owned system link requirement on either supported triplet; CI's generated link
and package metadata remain the final platform evidence.

## Resolved dependency edges

The pinned vcpkg `depend-info --format=dot` graph records all transitive edges. Its direct manifest
roots resolve as follows; repeated shared edges are represented once by the closure table above:

- `boost-asio -> boost-align, boost-assert, boost-cmake, boost-config, boost-context,
  boost-date-time, boost-headers, boost-system, boost-throw-exception`;
- `boost-beast -> boost-asio, boost-assert, boost-bind, boost-cmake, boost-config, boost-container,
  boost-container-hash, boost-core, boost-endian, boost-headers, boost-intrusive, boost-logic,
  boost-mp11, boost-optional, boost-smart-ptr, boost-static-string, boost-system,
  boost-throw-exception, boost-type-index, boost-type-traits, boost-winapi`;
- `boost-hash2 -> boost-assert, boost-cmake, boost-config, boost-container-hash, boost-describe,
  boost-headers, boost-mp11`;
- `boost-uuid -> boost-assert, boost-cmake, boost-config, boost-headers, boost-throw-exception,
  boost-type-traits`;
- `nlohmann-json -> vcpkg-cmake, vcpkg-cmake-config`;
- `boost-cmake -> boost-uninstall, vcpkg-boost`; and
- `vcpkg-boost -> vcpkg-cmake, vcpkg-cmake-config`.

The transitive Boost edges from the listed nodes select the remaining rows, including the
`boost-date-time` algorithm/function/range closure, the `boost-context` pool/integer closure, and
the shared Boost configuration, headers, exception, and type-traits closure. The CI `vcpkg list`
output is the authoritative platform evidence for the installed result; this document does not
constitute a hand-maintained lock file.

## Attribution

The direct `nlohmann-json` port and its source distribution require MIT attribution. The vcpkg build
helpers `boost-uninstall`, `vcpkg-boost`, `vcpkg-cmake`, and `vcpkg-cmake-config` require their MIT
terms. Boost.Beast, Boost.Asio, Boost.UUID, Boost.Hash2, and the complete Boost closure require the
Boost Software License 1.0 terms. Attribution text is sourced from the upstream distribution files
(`LICENSE`, `LICENSE_1_0.txt`) and the vcpkg port metadata at the pinned baseline. Distribution must
retain those notices and license text. This project does not add a project-level `NOTICE` file.
