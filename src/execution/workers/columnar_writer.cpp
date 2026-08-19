#include "borophene/execution/workers/columnar_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace borophene::execution {

using storage::ColumnChunkMetadata;
using storage::ColumnCompression;
using storage::ColumnEncoder;
using storage::ColumnEncoding;
using storage::kColumnarFormatFlags;
using storage::kColumnarFormatMajor;
using storage::kColumnarFormatMinor;
using storage::kColumnarHeaderMagic;
using storage::kColumnarHeaderSize;
using storage::kColumnarMaxChunkSize;
using storage::kColumnarMaxFieldCount;
using storage::kColumnarMaxFieldNameSize;
using storage::kColumnarMaxMetadataSize;
using storage::kColumnarMaxRowGroupCount;
using storage::kColumnarTrailerMagic;
using storage::kColumnarTrailerSize;
using storage::RowGroupMetadata;
using storage::SerializedColumn;
using storage::ValidityBitmapSize;

namespace {

using Buffer = std::vector<Byte>;

constexpr ui64 kMetadataHeaderSize = sizeof(ui32) + sizeof(ui32) + sizeof(ui64);
constexpr ui64 kFieldMetadataFixedSize = sizeof(ui32) + sizeof(ui8) + sizeof(ui8) + sizeof(ui16);
constexpr ui64 kRowGroupMetadataFixedSize = sizeof(ui64) + sizeof(ui32) + sizeof(ui32);
constexpr ui64 kChunkMetadataSize =
    sizeof(ui64) + sizeof(ui64) + sizeof(ui64) + sizeof(ui32) + sizeof(ui8) + sizeof(ui8) + sizeof(ui16);

Error InvalidState(std::string message) {
  return {ErrorCode::kInvalidState, std::move(message)};
}

Error InvalidArgument(std::string message) {
  return {ErrorCode::kInvalidArgument, std::move(message)};
}

Result<ui64> AddMetadataSize(ui64 current, ui64 increment) {
  if (current > kColumnarMaxMetadataSize || increment > kColumnarMaxMetadataSize - current) {
    return Failure<ui64>(ErrorCode::kInvalidArgument, "metadata exceeds the format limit");
  }
  return current + increment;
}

Result<ui64> InitialMetadataSize(const Schema& schema) {
  ui64 size = kMetadataHeaderSize;
  for (const Field& field : schema.Fields()) {
    auto with_fixed = AddMetadataSize(size, kFieldMetadataFixedSize);
    if (!with_fixed) {
      return MakeUnexpected(std::move(with_fixed.error()));
    }
    auto with_name = AddMetadataSize(*with_fixed, field.name.size());
    if (!with_name) {
      return MakeUnexpected(std::move(with_name.error()));
    }
    size = *with_name;
  }
  return size;
}

Result<ui64> AddRowGroupMetadataSize(ui64 current, Index column_count) {
  if (column_count > (kColumnarMaxMetadataSize - kRowGroupMetadataFixedSize) / kChunkMetadataSize) {
    return Failure<ui64>(ErrorCode::kInvalidArgument, "row-group metadata exceeds the format limit");
  }
  return AddMetadataSize(current, kRowGroupMetadataFixedSize + column_count * kChunkMetadataSize);
}

Result<void> WriteBytes(io::OutputStream& output, std::span<const Byte> bytes) {
  const ui64 position = output.Position();
  if (bytes.size() > std::numeric_limits<ui64>::max() - position) {
    return Failure<void>(ErrorCode::kOutOfRange, "output position overflows while writing columnar data");
  }
  auto write = output.Write(bytes);
  if (!write) {
    return MakeUnexpected(std::move(write.error()));
  }
  if (output.Position() != position + bytes.size()) {
    return Failure<void>(ErrorCode::kIo, "output stream did not advance by the written byte count");
  }
  return {};
}

Result<ui32> ValidateSourceColumn(const ColumnVector& column, const Field& field) {
  if (column.Type() != field.type) {
    return Failure<ui32>(ErrorCode::kSchemaMismatch, "row-group column type does not match the writer schema");
  }
  const Index null_count = column.Validity().NullCount();
  if (column.Validity().Size() != column.Size() || null_count > std::numeric_limits<ui32>::max()) {
    return Failure<ui32>(ErrorCode::kInvalidArgument, "source column has invalid validity metadata");
  }
  if (!field.nullable && null_count != 0) {
    return Failure<ui32>(ErrorCode::kSchemaMismatch, "a non-nullable source column contains null values");
  }
  return static_cast<ui32>(null_count);
}

Result<void> ValidateSerializedColumn(const SerializedColumn& serialized, const ColumnVector& column,
                                      const Field& field, ui32 expected_null_count) {
  if (serialized.null_count != expected_null_count) {
    return Failure<void>(ErrorCode::kInvalidArgument, "column encoder returned an invalid null count");
  }
  if (!field.nullable && serialized.null_count != 0) {
    return Failure<void>(ErrorCode::kSchemaMismatch, "column encoder returned nulls for a non-nullable field");
  }
  if (serialized.bytes.size() > kColumnarMaxChunkSize || serialized.decoded_size > kColumnarMaxChunkSize) {
    return Failure<void>(ErrorCode::kInvalidArgument, "column encoder returned a chunk that exceeds the format limit");
  }
  if (serialized.encoding != ColumnEncoding::kPlain) {
    return Failure<void>(ErrorCode::kUnsupportedVersion, "column encoder returned an unsupported encoding");
  }
  if (serialized.compression != ColumnCompression::kNone && serialized.compression != ColumnCompression::kZstd) {
    return Failure<void>(ErrorCode::kUnsupportedVersion, "column encoder returned an unsupported compression");
  }
  if (serialized.compression == ColumnCompression::kNone && serialized.decoded_size != serialized.bytes.size()) {
    return Failure<void>(ErrorCode::kInvalidArgument, "uncompressed column size does not match its decoded size");
  }

  const ui64 bitmap_size = expected_null_count == 0 ? 0 : ValidityBitmapSize(column.Size());
  const ui64 value_count = field.type == LogicalType::kString ? column.Size() + 1U : column.Size();
  if (value_count > (std::numeric_limits<ui64>::max() - bitmap_size) / sizeof(ui32)) {
    return Failure<void>(ErrorCode::kInvalidArgument, "column encoder returned an overflowing decoded size");
  }
  const ui64 minimum_size = bitmap_size + value_count * sizeof(ui32);
  if ((field.type == LogicalType::kInt32 && serialized.decoded_size != minimum_size) ||
      (field.type == LogicalType::kString && serialized.decoded_size < minimum_size)) {
    return Failure<void>(ErrorCode::kInvalidArgument, "column encoder returned an invalid decoded size");
  }
  return {};
}

void AppendU8(Buffer& output, ui8 value) {
  output.push_back(static_cast<Byte>(value));
}

void AppendU16(Buffer& output, ui16 value) {
  AppendU8(output, static_cast<ui8>(value));
  AppendU8(output, static_cast<ui8>(value >> 8U));
}

void AppendU32(Buffer& output, ui32 value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    AppendU8(output, static_cast<ui8>(value >> shift));
  }
}

void AppendU64(Buffer& output, ui64 value) {
  for (unsigned int shift = 0; shift < 64U; shift += 8U) {
    AppendU8(output, static_cast<ui8>(value >> shift));
  }
}

void AppendMagic(Buffer& output, const std::array<char, 8>& magic) {
  for (const char value : magic) {
    AppendU8(output, static_cast<ui8>(value));
  }
}

Result<Buffer> EncodeMetadata(const Schema& schema, const std::vector<RowGroupMetadata>& row_groups, Index total_rows,
                              ui64 expected_size) {
  if (schema.Size() > std::numeric_limits<ui32>::max() || row_groups.size() > std::numeric_limits<ui32>::max()) {
    return MakeUnexpected(InvalidArgument("metadata count exceeds format limit"));
  }

  Buffer output;
  output.reserve(static_cast<std::size_t>(expected_size));
  AppendU32(output, static_cast<ui32>(schema.Size()));
  AppendU32(output, static_cast<ui32>(row_groups.size()));
  AppendU64(output, total_rows);

  for (const Field& field : schema.Fields()) {
    if (field.name.size() > std::numeric_limits<ui32>::max()) {
      return MakeUnexpected(InvalidArgument("field name exceeds format limit"));
    }
    AppendU32(output, static_cast<ui32>(field.name.size()));
    for (const char byte : field.name) {
      AppendU8(output, static_cast<ui8>(byte));
    }
    AppendU8(output, static_cast<ui8>(field.type));
    AppendU8(output, field.nullable ? 1U : 0U);
    AppendU16(output, 0);
  }

  for (const RowGroupMetadata& group : row_groups) {
    if (group.chunks.size() > std::numeric_limits<ui32>::max()) {
      return MakeUnexpected(InvalidArgument("chunk count exceeds format limit"));
    }
    AppendU64(output, group.first_row);
    AppendU32(output, group.row_count);
    AppendU32(output, static_cast<ui32>(group.chunks.size()));
    for (const ColumnChunkMetadata& chunk : group.chunks) {
      AppendU64(output, chunk.offset);
      AppendU64(output, chunk.stored_size);
      AppendU64(output, chunk.decoded_size);
      AppendU32(output, chunk.null_count);
      AppendU8(output, static_cast<ui8>(chunk.encoding));
      AppendU8(output, static_cast<ui8>(chunk.compression));
      AppendU16(output, chunk.flags);
    }
  }
  if (output.size() != expected_size) {
    return Failure<Buffer>(ErrorCode::kInvalidState, "metadata size tracking disagrees with serialized metadata");
  }
  return output;
}

}  // namespace

ColumnarWriter::ColumnarWriter(std::unique_ptr<io::OutputStream> output,
                               std::shared_ptr<const ColumnEncoder> encoder) noexcept
    : output_(std::move(output)), encoder_(std::move(encoder)) {
}

Result<ColumnarWriter> ColumnarWriter::Create(std::unique_ptr<io::OutputStream> output,
                                              std::shared_ptr<const ColumnEncoder> encoder) {
  if (output == nullptr) {
    return MakeUnexpected(InvalidArgument("output stream must not be null"));
  }
  if (encoder == nullptr) {
    return MakeUnexpected(InvalidArgument("column encoder must not be null"));
  }
  return ColumnarWriter(std::move(output), std::move(encoder));
}

Result<void> ColumnarWriter::Begin(const Schema& schema) {
  if (state_ != State::kCreated) {
    return MakeUnexpected(InvalidState("writer has already been started"));
  }
  if (output_->Position() != 0) {
    return MakeUnexpected(InvalidArgument("output stream must start at position zero"));
  }
  if (schema.Size() > kColumnarMaxFieldCount) {
    return MakeUnexpected(InvalidArgument("schema has too many fields"));
  }
  for (const Field& field : schema.Fields()) {
    if (field.name.size() > kColumnarMaxFieldNameSize) {
      return MakeUnexpected(InvalidArgument("field name exceeds the format limit"));
    }
    if (field.type != LogicalType::kInt32 && field.type != LogicalType::kString) {
      return MakeUnexpected(InvalidArgument("schema contains an unsupported logical type"));
    }
  }

  auto metadata_size = InitialMetadataSize(schema);
  if (!metadata_size) {
    return MakeUnexpected(std::move(metadata_size.error()));
  }

  Buffer header;
  header.reserve(static_cast<std::size_t>(kColumnarHeaderSize));
  AppendMagic(header, kColumnarHeaderMagic);
  AppendU16(header, kColumnarFormatMajor);
  AppendU16(header, kColumnarFormatMinor);
  AppendU32(header, kColumnarFormatFlags);

  auto write = WriteBytes(*output_, header);
  if (!write) {
    state_ = State::kFailed;
    return MakeUnexpected(std::move(write.error()));
  }
  schema_ = schema;
  metadata_size_ = *metadata_size;
  state_ = State::kBegun;
  return {};
}

Result<void> ColumnarWriter::Write(const DataChunk& chunk) {
  return WriteRowGroup(chunk);
}

Result<void> ColumnarWriter::WriteRowGroup(const DataChunk& chunk) {
  if (state_ != State::kBegun) {
    return MakeUnexpected(InvalidState("writer must be begun and unfinished"));
  }
  if (!schema_.has_value()) {
    return MakeUnexpected(InvalidState("writer schema is unavailable"));
  }
  if (chunk.RowCount() == 0) {
    return MakeUnexpected(InvalidArgument("empty row groups are not allowed"));
  }
  if (chunk.RowCount() > std::numeric_limits<ui32>::max()) {
    return MakeUnexpected(InvalidArgument("row group exceeds format limit"));
  }
  if (chunk.Columns().size() != schema_->Size()) {
    return MakeUnexpected(Error(ErrorCode::kSchemaMismatch, "row-group column count does not match the writer schema"));
  }
  if (row_groups_.size() == kColumnarMaxRowGroupCount) {
    return MakeUnexpected(InvalidArgument("too many row groups"));
  }
  if (chunk.RowCount() > std::numeric_limits<Index>::max() - total_rows_) {
    return MakeUnexpected(InvalidArgument("total row count overflow"));
  }
  auto next_metadata_size = AddRowGroupMetadataSize(metadata_size_, schema_->Size());
  if (!next_metadata_size) {
    return MakeUnexpected(std::move(next_metadata_size.error()));
  }

  std::vector<SerializedColumn> serialized_columns;
  serialized_columns.reserve(chunk.Columns().size());
  for (Index index = 0; index < schema_->Size(); ++index) {
    auto column = chunk.Column(index);
    if (!column) {
      return MakeUnexpected(std::move(column.error()));
    }
    auto field = schema_->FieldAt(index);
    if (!field) {
      return MakeUnexpected(std::move(field.error()));
    }
    auto expected_null_count = ValidateSourceColumn(column->get(), field->get());
    if (!expected_null_count) {
      return MakeUnexpected(std::move(expected_null_count.error()));
    }
    auto serialized = encoder_->Serialize(column->get(), field->get());
    if (!serialized) {
      return MakeUnexpected(std::move(serialized.error()));
    }
    auto validation = ValidateSerializedColumn(*serialized, column->get(), field->get(), *expected_null_count);
    if (!validation) {
      return MakeUnexpected(std::move(validation.error()));
    }
    serialized_columns.push_back(std::move(*serialized));
  }

  RowGroupMetadata group;
  group.first_row = total_rows_;
  group.row_count = static_cast<ui32>(chunk.RowCount());
  group.chunks.reserve(serialized_columns.size());
  for (const SerializedColumn& serialized : serialized_columns) {
    ColumnChunkMetadata metadata;
    metadata.offset = output_->Position();
    metadata.stored_size = serialized.bytes.size();
    metadata.decoded_size = serialized.decoded_size;
    metadata.null_count = serialized.null_count;
    metadata.encoding = serialized.encoding;
    metadata.compression = serialized.compression;

    auto write = WriteBytes(*output_, serialized.bytes);
    if (!write) {
      state_ = State::kFailed;
      return MakeUnexpected(std::move(write.error()));
    }
    group.chunks.push_back(metadata);
  }

  row_groups_.push_back(std::move(group));
  total_rows_ += chunk.RowCount();
  metadata_size_ = *next_metadata_size;
  return {};
}

Result<void> ColumnarWriter::Finish() {
  if (state_ != State::kBegun) {
    return MakeUnexpected(InvalidState("writer must be begun and unfinished"));
  }
  if (!schema_.has_value()) {
    return MakeUnexpected(InvalidState("writer schema is unavailable"));
  }

  auto metadata = EncodeMetadata(*schema_, row_groups_, total_rows_, metadata_size_);
  if (!metadata) {
    return MakeUnexpected(std::move(metadata.error()));
  }
  if (metadata->size() > kColumnarMaxMetadataSize) {
    return MakeUnexpected(InvalidArgument("metadata exceeds the format limit"));
  }
  const ui64 metadata_offset = output_->Position();
  if (metadata_offset > std::numeric_limits<ui64>::max() - kColumnarTrailerSize ||
      metadata->size() > std::numeric_limits<ui64>::max() - metadata_offset - kColumnarTrailerSize) {
    return MakeUnexpected(InvalidArgument("file size overflow"));
  }
  const ui64 file_size = metadata_offset + metadata->size() + kColumnarTrailerSize;

  auto write_metadata = WriteBytes(*output_, *metadata);
  if (!write_metadata) {
    state_ = State::kFailed;
    return MakeUnexpected(std::move(write_metadata.error()));
  }

  Buffer trailer;
  trailer.reserve(static_cast<std::size_t>(kColumnarTrailerSize));
  AppendMagic(trailer, kColumnarTrailerMagic);
  AppendU16(trailer, kColumnarFormatMajor);
  AppendU16(trailer, kColumnarFormatMinor);
  AppendU32(trailer, kColumnarFormatFlags);
  AppendU64(trailer, metadata_offset);
  AppendU64(trailer, metadata->size());
  AppendU64(trailer, file_size);
  auto write_trailer = WriteBytes(*output_, trailer);
  if (!write_trailer) {
    state_ = State::kFailed;
    return MakeUnexpected(std::move(write_trailer.error()));
  }

  auto flush = output_->Flush();
  if (!flush) {
    state_ = State::kFailed;
    return MakeUnexpected(std::move(flush.error()));
  }
  state_ = State::kFinished;
  return {};
}

}  // namespace borophene::execution
