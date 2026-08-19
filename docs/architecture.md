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
data ------> common
io --------> common
storage ---> data
execution -> data
execution -> io
execution -> storage
```

`common` contains value-level infrastructure. `data` owns typed vectors and validates table invariants. `io` contains
byte-stream contracts, memory/local adapters, and schema-independent CSV logic. `storage` owns the columnar format
metadata and schema-aware column codec, depending only on `data`. `execution` coordinates sources and sinks; its
columnar workers compose the storage codec with injected I/O contracts. Dependencies point in one direction and the
storage layer has no dependency back to execution.

Each module is a CMake target with public dependency propagation. The `borophene::borophene` interface target is the
consumer entry point.

## Ownership and errors

- `Schema`, `ColumnVector`, and `DataChunk` own their state. No process-global registry is used.
- File and memory adapters are owned independently from the workers that consume their virtual stream contracts.
- A chunk passed to a sink is borrowed only for the duration of `Write`.
- Input, I/O, bounds, type, and format failures use `Result<T>` (`std::expected<T, Error>`).
- Borophene APIs do not explicitly throw for validation or state errors; those failures use `Result`. Standard
  allocation failures retain their normal C++ behavior.
- End of input is `Result<std::optional<DataChunk>>` with `nullopt`, never an empty chunk or an error.

## Execution

`ChunkSource` exposes an immutable schema and produces one chunk at a time. `ChunkSink` has an explicit
`Begin`/`Write`/`Finish` lifecycle. `RunPipeline` propagates the first error and only calls `Finish` after a clean end of
input.

Future operators should implement `ChunkSource` and uniquely own their upstream source. This keeps the execution graph
acyclic and avoids shared mutable column state.

## Execution workers, storage, and CSV

The CSV parser is schema-agnostic and depends only on sequential `InputStream`/`OutputStream` contracts. Local files and
the reusable `MemoryStream` implement those contracts without leaking a destination type into CSV workers.

Columnar I/O is a composition of small stages. The reader execution worker obtains payload bytes through a
random-access input and passes them to the storage codec; the codec alone validates and constructs a `ColumnVector`.
In the opposite direction, the codec serializes a vector into bytes and metadata before the writer execution worker
sends those bytes to an injected output stream. Workers orchestrate lifecycle and row groups, while codecs do not know
which file or memory backend is in use. This boundary is also where a future compression implementation is selected.

The reader worker uses positional reads so projected column chunks can later be read concurrently. Metadata lives at
the end of the file, allowing the writer worker to emit row groups in one pass without seeking.

The local file adapter currently targets POSIX systems. Its abstract random-access and sequential stream contracts keep
platform-specific file handling outside CSV, columnar execution workers, and codecs.

All external bytes are validated before allocation or indexing. The v1 format uses fixed-width little-endian fields;
it never serializes pointers, `size_t`, C++ object layouts, or platform-specific numeric types.

## Deferred work

- SQL parsing and logical/physical planning.
- Filter, projection, aggregation, ordering, and join operators.
- Built-in compression implementations, statistics, checksums, and predicate pushdown. The codec contract already
  isolates the compression boundary; the built-in v1 codec currently implements only `kNone`, while the format
  reserves `kZstd` for an injected encoder/factory pair.
- Schema inference and typed CSV import policy.
- Memory mapping, object-storage backends, and parallel ingestion.
- Additional logical types beyond `INT32` and `STRING`.

These features should be introduced only with working implementations and tests, not empty placeholder classes.
