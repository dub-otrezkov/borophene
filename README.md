# Borophene

Borophene is an experimental C++23 columnar storage engine. The current foundation provides owned column vectors,
validated schemas and data chunks, a pull-based execution pipeline, streaming CSV I/O, and a versioned columnar file
format.

The current local file backend requires a POSIX-compatible platform. Linux and macOS are covered by CI.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBOROPHENE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Strict local checks can be enabled without imposing those flags on downstream consumers:

```bash
cmake -S . -B build-quality \
  -DBOROPHENE_BUILD_TESTS=ON \
  -DBOROPHENE_WARNINGS_AS_ERRORS=ON \
  -DBOROPHENE_ENABLE_CLANG_TIDY=ON
cmake --build build-quality --parallel
cmake --build build-quality --target borophene-format-check
ctest --test-dir build-quality --output-on-failure
```

## Design

The engine is split into dependency-directed modules:

- `borophene::common`: error/result primitives and low-level value types.
- `borophene::data`: logical types, schemas, owned column vectors, and data chunks.
- `borophene::io`: streaming CSV and local file abstractions.
- `borophene::storage`: the Borophene v1 columnar reader and writer.
- `borophene::execution`: source/sink contracts and pull pipeline orchestration.

See [Architecture](docs/architecture.md) and [Columnar format v1](docs/columnar-format-v1.md) for the contracts and
validation rules.
