#include "borophene/storage/columnar_writer.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace borophene::storage {
namespace {

using Buffer = std::vector<std::byte>;

[[nodiscard]] Error InvalidState(std::string message) {
  return {ErrorCode::kInvalidState, std::move(message)};
}

[[nodiscard]] Error InvalidArgument(std::string message) {
  return {ErrorCode::kInvalidArgument, std::move(message)};
}

void AppendU8(Buffer& output, std::uint8_t value) {
  output.push_back(static_cast<std::byte>(value));
}

void AppendU16(Buffer& output, std::uint16_t value) {
  AppendU8(output, static_cast<std::uint8_t>(value));
  AppendU8(output, static_cast<std::uint8_t>(value >> 8U));
}

void AppendU32(Buffer& output, std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    AppendU8(output, static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendU64(Buffer& output, std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64U; shift += 8U) {
    AppendU8(output, static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendMagic(Buffer& output, const std::array<char, 8>& magic) {
  for (const char value : magic) {
    AppendU8(output, static_cast<std::uint8_t>(value));
  }
}

[[nodiscard]] Result<Buffer> EncodeColumn(const ColumnVector& column, const Field& field) {
  if (column.Type() != field.type) {
    return std::unexpected(Error(ErrorCode::kSchemaMismatch, "row-group column type does not match the writer schema"));
  }
  if (column.Validity().Size() != column.Size()) {
    return std::unexpected(InvalidArgument("column validity size mismatch"));
  }
  if (!field.nullable && column.Validity().NullCount() != 0) {
    return std::unexpected(Error(ErrorCode::kSchemaMismatch, "a non-nullable column contains null values"));
  }
  if (column.Validity().NullCount() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(InvalidArgument("column has too many null values"));
  }

  Buffer output;
  const Index row_count = column.Size();
  if (column.Validity().NullCount() != 0) {
    const std::uint64_t bitmap_size = ValidityBitmapSize(row_count);
    if (bitmap_size > output.max_size()) {
      return std::unexpected(InvalidArgument("validity bitmap is too large"));
    }
    output.resize(static_cast<std::size_t>(bitmap_size));
    for (Index row = 0; row < row_count; ++row) {
      if (column.Validity().IsValid(row)) {
        const auto bit = static_cast<std::uint8_t>(1U << (row % 8U));
        output[static_cast<std::size_t>(row / 8U)] |= static_cast<std::byte>(bit);
      }
    }
  }

  if (field.type == LogicalType::kInt32) {
    const auto& values = column.Int32Values();
    if (values.size() != row_count || row_count > (output.max_size() - output.size()) / sizeof(std::int32_t)) {
      return std::unexpected(InvalidArgument("int32 column is too large"));
    }
    output.reserve(output.size() + values.size() * sizeof(std::int32_t));
    for (Index row = 0; row < row_count; ++row) {
      const std::int32_t value = column.Validity().IsValid(row) ? values[static_cast<std::size_t>(row)] : 0;
      AppendU32(output, std::bit_cast<std::uint32_t>(value));
    }
    return output;
  }

  if (field.type != LogicalType::kString) {
    return std::unexpected(InvalidArgument("unsupported logical type"));
  }

  const auto& values = column.StringValues();
  if (values.size() != row_count || row_count == std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(InvalidArgument("string column is too large"));
  }

  const std::uint64_t offset_count = row_count + 1U;
  if (offset_count > (output.max_size() - output.size()) / sizeof(std::uint32_t)) {
    return std::unexpected(InvalidArgument("string offset table is too large"));
  }

  std::vector<std::uint32_t> offsets;
  offsets.reserve(static_cast<std::size_t>(offset_count));
  offsets.push_back(0);
  std::uint64_t byte_count = 0;
  for (Index row = 0; row < row_count; ++row) {
    if (column.Validity().IsValid(row)) {
      const auto& value = values[static_cast<std::size_t>(row)];
      if (value.size() > std::numeric_limits<std::uint32_t>::max() - byte_count) {
        return std::unexpected(InvalidArgument("string data exceeds the format limit"));
      }
      byte_count += value.size();
    }
    offsets.push_back(static_cast<std::uint32_t>(byte_count));
  }

  const std::uint64_t offset_bytes = offset_count * sizeof(std::uint32_t);
  if (byte_count > output.max_size() - output.size() - offset_bytes) {
    return std::unexpected(InvalidArgument("string column is too large"));
  }
  output.reserve(output.size() + static_cast<std::size_t>(offset_bytes + byte_count));
  for (const std::uint32_t offset : offsets) {
    AppendU32(output, offset);
  }
  for (Index row = 0; row < row_count; ++row) {
    if (!column.Validity().IsValid(row)) {
      continue;
    }
    const auto& value = values[static_cast<std::size_t>(row)];
    for (const char byte : value) {
      AppendU8(output, static_cast<std::uint8_t>(byte));
    }
  }
  return output;
}

[[nodiscard]] Result<Buffer> EncodeMetadata(const Schema& schema, const std::vector<RowGroupMetadata>& row_groups,
                                            Index total_rows) {
  if (schema.Size() > std::numeric_limits<std::uint32_t>::max() ||
      row_groups.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(InvalidArgument("metadata count exceeds format limit"));
  }

  Buffer output;
  AppendU32(output, static_cast<std::uint32_t>(schema.Size()));
  AppendU32(output, static_cast<std::uint32_t>(row_groups.size()));
  AppendU64(output, total_rows);

  for (const Field& field : schema.Fields()) {
    if (field.name.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InvalidArgument("field name exceeds format limit"));
    }
    AppendU32(output, static_cast<std::uint32_t>(field.name.size()));
    for (const char byte : field.name) {
      AppendU8(output, static_cast<std::uint8_t>(byte));
    }
    AppendU8(output, static_cast<std::uint8_t>(field.type));
    AppendU8(output, field.nullable ? 1U : 0U);
    AppendU16(output, 0);
  }

  for (const RowGroupMetadata& group : row_groups) {
    if (group.chunks.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(InvalidArgument("chunk count exceeds format limit"));
    }
    AppendU64(output, group.first_row);
    AppendU32(output, group.row_count);
    AppendU32(output, static_cast<std::uint32_t>(group.chunks.size()));
    for (const ColumnChunkMetadata& chunk : group.chunks) {
      AppendU64(output, chunk.offset);
      AppendU64(output, chunk.stored_size);
      AppendU64(output, chunk.decoded_size);
      AppendU32(output, chunk.null_count);
      AppendU8(output, static_cast<std::uint8_t>(chunk.encoding));
      AppendU8(output, static_cast<std::uint8_t>(chunk.compression));
      AppendU16(output, chunk.flags);
    }
  }
  return output;
}

}  // namespace

ColumnarWriter::ColumnarWriter(std::unique_ptr<io::OutputFile> file) noexcept : file_(std::move(file)) {}

Result<ColumnarWriter> ColumnarWriter::Create(std::unique_ptr<io::OutputFile> file) {
  if (file == nullptr) {
    return std::unexpected(InvalidArgument("output file must not be null"));
  }
  return ColumnarWriter(std::move(file));
}

Result<ColumnarWriter> ColumnarWriter::Create(const std::filesystem::path& path) {
  auto file = io::CreateLocalOutput(path);
  if (!file) {
    return std::unexpected(std::move(file.error()));
  }
  return Create(std::move(*file));
}

Result<void> ColumnarWriter::Begin(const Schema& schema) {
  if (state_ != State::kCreated) {
    return std::unexpected(InvalidState("writer has already been started"));
  }
  if (file_->Position() != 0) {
    return std::unexpected(InvalidArgument("output file must be empty"));
  }
  if (schema.Size() > kColumnarMaxFieldCount) {
    return std::unexpected(InvalidArgument("schema has too many fields"));
  }
  for (const Field& field : schema.Fields()) {
    if (field.name.size() > kColumnarMaxFieldNameSize) {
      return std::unexpected(InvalidArgument("field name exceeds the format limit"));
    }
    if (field.type != LogicalType::kInt32 && field.type != LogicalType::kString) {
      return std::unexpected(InvalidArgument("schema contains an unsupported logical type"));
    }
  }

  Buffer header;
  header.reserve(static_cast<std::size_t>(kColumnarHeaderSize));
  AppendMagic(header, kColumnarHeaderMagic);
  AppendU16(header, kColumnarFormatMajor);
  AppendU16(header, kColumnarFormatMinor);
  AppendU32(header, kColumnarFormatFlags);

  auto write = file_->Write(header);
  if (!write) {
    state_ = State::kFailed;
    return std::unexpected(std::move(write.error()));
  }
  schema_ = schema;
  state_ = State::kBegun;
  return {};
}

Result<void> ColumnarWriter::Write(const DataChunk& chunk) {
  return WriteRowGroup(chunk);
}

Result<void> ColumnarWriter::WriteRowGroup(const DataChunk& chunk) {
  if (state_ != State::kBegun) {
    return std::unexpected(InvalidState("writer must be begun and unfinished"));
  }
  if (!schema_.has_value()) {
    return std::unexpected(InvalidState("writer schema is unavailable"));
  }
  if (chunk.RowCount() == 0) {
    return std::unexpected(InvalidArgument("empty row groups are not allowed"));
  }
  if (chunk.RowCount() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(InvalidArgument("row group exceeds format limit"));
  }
  if (chunk.Columns().size() != schema_->Size()) {
    return std::unexpected(
        Error(ErrorCode::kSchemaMismatch, "row-group column count does not match the writer schema"));
  }
  if (row_groups_.size() == kColumnarMaxRowGroupCount) {
    return std::unexpected(InvalidArgument("too many row groups"));
  }
  if (chunk.RowCount() > std::numeric_limits<Index>::max() - total_rows_) {
    return std::unexpected(InvalidArgument("total row count overflow"));
  }

  std::vector<Buffer> encoded_columns;
  encoded_columns.reserve(chunk.Columns().size());
  for (Index index = 0; index < schema_->Size(); ++index) {
    auto encoded = EncodeColumn(chunk.Column(index), (*schema_)[index]);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    if (encoded->size() > kColumnarMaxChunkSize) {
      return std::unexpected(InvalidArgument("encoded column chunk exceeds the format limit"));
    }
    encoded_columns.push_back(std::move(*encoded));
  }

  RowGroupMetadata group;
  group.first_row = total_rows_;
  group.row_count = static_cast<std::uint32_t>(chunk.RowCount());
  group.chunks.reserve(encoded_columns.size());
  for (std::size_t index = 0; index < encoded_columns.size(); ++index) {
    const Buffer& encoded = encoded_columns[index];
    ColumnChunkMetadata metadata;
    metadata.offset = file_->Position();
    metadata.stored_size = encoded.size();
    metadata.decoded_size = encoded.size();
    metadata.null_count = static_cast<std::uint32_t>(chunk.Column(index).Validity().NullCount());

    auto write = file_->Write(encoded);
    if (!write) {
      state_ = State::kFailed;
      return std::unexpected(std::move(write.error()));
    }
    group.chunks.push_back(metadata);
  }

  row_groups_.push_back(std::move(group));
  total_rows_ += chunk.RowCount();
  return {};
}

Result<void> ColumnarWriter::Finish() {
  if (state_ != State::kBegun) {
    return std::unexpected(InvalidState("writer must be begun and unfinished"));
  }
  if (!schema_.has_value()) {
    return std::unexpected(InvalidState("writer schema is unavailable"));
  }

  auto metadata = EncodeMetadata(*schema_, row_groups_, total_rows_);
  if (!metadata) {
    return std::unexpected(std::move(metadata.error()));
  }
  if (metadata->size() > kColumnarMaxMetadataSize) {
    return std::unexpected(InvalidArgument("metadata exceeds the format limit"));
  }
  const std::uint64_t metadata_offset = file_->Position();
  if (metadata_offset > std::numeric_limits<std::uint64_t>::max() - kColumnarTrailerSize ||
      metadata->size() > std::numeric_limits<std::uint64_t>::max() - metadata_offset - kColumnarTrailerSize) {
    return std::unexpected(InvalidArgument("file size overflow"));
  }
  const std::uint64_t file_size = metadata_offset + metadata->size() + kColumnarTrailerSize;

  auto write_metadata = file_->Write(*metadata);
  if (!write_metadata) {
    state_ = State::kFailed;
    return std::unexpected(std::move(write_metadata.error()));
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
  auto write_trailer = file_->Write(trailer);
  if (!write_trailer) {
    state_ = State::kFailed;
    return std::unexpected(std::move(write_trailer.error()));
  }

  auto flush = file_->Flush();
  if (!flush) {
    state_ = State::kFailed;
    return std::unexpected(std::move(flush.error()));
  }
  state_ = State::kFinished;
  return {};
}

}  // namespace borophene::storage
