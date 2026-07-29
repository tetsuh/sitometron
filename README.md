# sitometron

Sitometron is a cross-platform controller for admitting, scheduling, supervising, and observing
trusted local compute applications. It is designed as an application-agnostic control plane and
keeps computation graphs, parameter interpretation, and artifact payloads outside its core.

The project is in its bootstrap phase. No production API or compatibility guarantee exists yet.

## Design boundaries

`sitometron_core` is a C++20, standard-library-only domain library. It does not depend on HTTP,
Sitos, Zenoh, Holoscan, Python, logging backends, hardware-topology libraries, or platform process
APIs. Adapters are composed only by `sitometrond`.

See:

- [Architecture boundaries](docs/architecture/boundaries.md)
- [Clean-room policy](docs/clean-room-policy.md)
- [Development workflow](docs/development_workflow.md)
- [Dependency policy](docs/dependency_policy.md)
- [Contract registry](docs/contract_registry.md)

## Build

Requirements:

- CMake 3.28 or later
- Ninja
- a C++20 compiler

Linux:

```sh
cmake --preset dev-linux
cmake --build --preset dev-linux
ctest --preset dev-linux
```

Windows:

```powershell
cmake --preset dev-windows
cmake --build --preset dev-windows
ctest --preset dev-windows
```

In-source builds are rejected.

## License

Apache License 2.0. See [LICENSE](LICENSE).
