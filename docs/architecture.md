# Architecture

## Goals

The first Borophene architecture establishes ownership, module boundaries, and error contracts before query operators
are added. It borrows the useful row-group pipeline shape from
[Columnar-DB-Engine at `fe1813f`](https://github.com/dub-otrezkov/Columnar-DB-Engine/tree/fe1813f6d76a43254ce3c662e08c764ff9b25bcd)
and the streaming/footer-indexed I/O mechanisms from
[ColumnarEngine at `cc750dc`](https://github.com/RagnarOkk11/ColumnarEngine/tree/cc750dc2f0e564fbd60ab739664aae0a7c094ee1),
but the implementation and on-disk format are independent.

The reference repositories do not declare a license. Their source is therefore not copied into Borophene.

## Dependency graph

```text
common <--- data <--- execution
   ^         ^           ^
   |         |           |
   +--- io <-+-----------+--- storage
```

`common` contains value-level infrastructure. `data` owns typed vectors and validates table invariants. `io` contains
byte and CSV streams without storage-engine knowledge. `execution` only knows the data model and coordinates sources
and sinks. `storage` depends on all three and implements the execution source/sink contracts for columnar files.

Each module is a CMake target with public dependency propagation. The `borophene::borophene` interface target is the
consumer entry point.

## Ownership and errors

- `Schema`, `ColumnVector`, and `DataChunk` own their state. No process-global registry is used.
- File readers and writers have unique ownership and deterministic close semantics.
- A chunk passed to a sink is borrowed only for the duration of `Write`.
- Expected input, I/O, and format failures use `Result<T>` (`std::expected<T, Error>`).
- Exceptions are reserved for programmer errors such as requesting the wrong typed vector view.
- End of input is `Result<std::optional<DataChunk>>` with `nullopt`, never an empty chunk or an error.

## Execution

`ChunkSource` exposes an immutable schema and produces one chunk at a time. `ChunkSink` has an explicit
`Begin`/`Write`/`Finish` lifecycle. `RunPipeline` propagates the first error and only calls `Finish` after a clean end of
input.

Future operators should implement `ChunkSource` and uniquely own their upstream source. This keeps the execution graph
acyclic and avoids shared mutable column state.

## Storage and CSV

The CSV parser is schema-agnostic and streaming. The storage layer uses positional reads so projected column chunks can
later be read concurrently. Metadata lives at the end of the file, allowing the writer to emit row groups in one pass
without seeking.

The local file adapter currently targets POSIX systems. Its abstract random-access and sequential-output contracts keep
platform-specific file handling outside the format reader and writer.

All external bytes are validated before allocation or indexing. The v1 format uses fixed-width little-endian fields;
it never serializes pointers, `size_t`, C++ object layouts, or platform-specific numeric types.

## Deferred work

- SQL parsing and logical/physical planning.
- Filter, projection, aggregation, ordering, and join operators.
- Compression, statistics, checksums, and predicate pushdown.
- Schema inference and typed CSV import policy.
- Memory mapping, object-storage backends, and parallel ingestion.
- Additional logical types beyond `INT32` and `STRING`.

These features should be introduced only with working implementations and tests, not empty placeholder classes.
