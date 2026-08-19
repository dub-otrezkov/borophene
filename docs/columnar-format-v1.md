# Borophene Columnar Format v1

All integers are unsigned little-endian unless stated otherwise. Sizes and offsets are measured in bytes. Readers must
reject unknown versions, flags, encodings, compression identifiers, and logical type identifiers.

## File layout

```text
[16-byte header]
[row group 0, column 0 payload]
[row group 0, column 1 payload]
...
[metadata]
[40-byte trailer]
```

The header contains the eight-byte `BOROPHEN` signature, `u16 major`, `u16 minor`, and `u32 flags`. Version 1.0 requires
all flags to be zero.

The trailer contains the eight-byte `BORO_END` signature, the same version and flags, followed by `u64 metadata_offset`,
`u64 metadata_size`, and `u64 file_size`.

## Metadata

Metadata begins with `u32 field_count`, `u32 row_group_count`, and `u64 total_rows`.

Each field stores `u32 name_size`, opaque name bytes, `u8 logical_type`, `u8 nullable`, and `u16 flags`. Names must be
non-empty and unique. Version 1 supports `INT32` (id 1) and `STRING` (id 2); field flags are zero. Text-oriented clients
may impose their own encoding policy without changing the byte-level format.

Each row group stores `u64 first_row`, `u32 row_count`, and `u32 chunk_count`, followed by one chunk descriptor per
field. A descriptor contains `u64 offset`, `u64 stored_size`, `u64 decoded_size`, `u32 null_count`, `u8 encoding`,
`u8 compression`, and `u16 flags`. Version 1 supports plain encoding, no compression, and zero flags.

Row groups must be non-empty, contiguous, and sum to `total_rows`. Every payload range must lie between the header and
metadata and must not overflow.

Version 1 resource limits are 16,384 fields, 1,000,000 row groups, 1 MiB per field name, 256 MiB of metadata, and 1 GiB
per encoded column chunk. Writers reject larger inputs and readers enforce the same limits before allocation.

## Plain payloads

If a chunk has nulls, its payload starts with `ceil(row_count / 8)` validity bytes. Bit `i` is one when row `i` is valid;
unused high bits in the last byte are zero.

`INT32` stores one little-endian 32-bit value per row. The encoded value for a null row is zero.

`STRING` stores `row_count + 1` little-endian `u32` offsets followed by bytes. The first offset is zero, offsets are
monotonic, and the last offset equals the byte region size. A null row repeats its preceding offset, preserving the
difference between null and an empty valid string through the validity bitmap.

## Validation

Readers validate both signatures, matching header/trailer versions, exact file and metadata bounds, bounded field and
row-group counts, complete metadata consumption, payload sizes, validity padding, and string offsets before exposing a
chunk. Truncated or malformed data produces a typed error rather than partial data or undefined behavior.
