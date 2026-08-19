#include "borophene/execution/workers/columnar_reader.hpp"

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
using storage::ColumnDecodeOptions;
using storage::ColumnEncoding;
using storage::ColumnFactory;
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
using storage::ValidityBitmapSize;

namespace {

using Buffer = std::vector<Byte>;

Error InvalidFormat(std::string message) {
  return {ErrorCode::kInvalidFormat, std::move(message)};
}

bool CheckedAdd(ui64 left, ui64 right, ui64& result) noexcept {
  if (right > std::numeric_limits<ui64>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

class Cursor {
 public:
  explicit Cursor(std::span<const Byte> bytes) : bytes_(bytes) {
  }

  Result<ui8> ReadU8() {
    if (position_ == bytes_.size()) {
      return MakeUnexpected(InvalidFormat("truncated columnar metadata"));
    }
    return std::to_integer<ui8>(bytes_[position_++]);
  }

  Result<ui16> ReadU16() {
    auto bytes = ReadBytes(sizeof(ui16));
    if (!bytes) {
      return MakeUnexpected(std::move(bytes.error()));
    }
    return static_cast<ui16>(std::to_integer<ui8>((*bytes)[0]) |
                             (static_cast<ui16>(std::to_integer<ui8>((*bytes)[1])) << 8U));
  }

  Result<ui32> ReadU32() {
    auto bytes = ReadBytes(sizeof(ui32));
    if (!bytes) {
      return MakeUnexpected(std::move(bytes.error()));
    }
    ui32 value = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
      value |= static_cast<ui32>(std::to_integer<ui8>((*bytes)[index])) << (index * 8U);
    }
    return value;
  }

  Result<ui64> ReadU64() {
    auto bytes = ReadBytes(sizeof(ui64));
    if (!bytes) {
      return MakeUnexpected(std::move(bytes.error()));
    }
    ui64 value = 0;
    for (unsigned int index = 0; index < 8U; ++index) {
      value |= static_cast<ui64>(std::to_integer<ui8>((*bytes)[index])) << (index * 8U);
    }
    return value;
  }

  Result<std::span<const Byte>> ReadBytes(ui64 size) {
    if (size > bytes_.size() - position_) {
      return MakeUnexpected(InvalidFormat("truncated columnar metadata"));
    }
    const auto output = bytes_.subspan(position_, static_cast<std::size_t>(size));
    position_ += static_cast<std::size_t>(size);
    return output;
  }

  bool Empty() const noexcept {
    return position_ == bytes_.size();
  }
  std::size_t Remaining() const noexcept {
    return bytes_.size() - position_;
  }

 private:
  std::span<const Byte> bytes_;
  std::size_t position_ = 0;
};

bool MatchesMagic(std::span<const Byte> bytes, const std::array<char, 8>& magic) {
  if (bytes.size() != magic.size()) {
    return false;
  }
  for (std::size_t index = 0; index < magic.size(); ++index) {
    if (std::to_integer<ui8>(bytes[index]) != static_cast<ui8>(magic[index])) {
      return false;
    }
  }
  return true;
}

Result<Buffer> ReadRegion(const io::RandomAccessFile& file, ui64 offset, ui64 size) {
  if (size > std::numeric_limits<std::size_t>::max()) {
    return MakeUnexpected(InvalidFormat("columnar region is too large"));
  }
  Buffer bytes(static_cast<std::size_t>(size));
  auto read = io::ReadExactly(file, offset, bytes);
  if (!read) {
    return MakeUnexpected(std::move(read.error()));
  }
  return bytes;
}

struct Preamble {
  ui16 major;
  ui16 minor;
  ui32 flags;
};

Result<Preamble> ParsePreamble(std::span<const Byte> bytes, const std::array<char, 8>& magic,
                               std::string_view location) {
  Cursor cursor(bytes);
  auto encoded_magic = cursor.ReadBytes(magic.size());
  if (!encoded_magic || !MatchesMagic(*encoded_magic, magic)) {
    return MakeUnexpected(InvalidFormat(std::string(location) + " magic is invalid"));
  }
  auto major = cursor.ReadU16();
  auto minor = cursor.ReadU16();
  auto flags = cursor.ReadU32();
  if (!major || !minor || !flags) {
    return MakeUnexpected(InvalidFormat(std::string(location) + " is truncated"));
  }
  if (*major != kColumnarFormatMajor || *minor != kColumnarFormatMinor) {
    return MakeUnexpected(
        Error(ErrorCode::kUnsupportedVersion,
              "unsupported columnar format version " + std::to_string(*major) + "." + std::to_string(*minor)));
  }
  if (*flags != kColumnarFormatFlags) {
    return MakeUnexpected(InvalidFormat(std::string(location) + " has unknown flags"));
  }
  return Preamble{.major = *major, .minor = *minor, .flags = *flags};
}

struct ParsedMetadata {
  Schema schema;
  std::vector<RowGroupMetadata> row_groups;
  Index total_rows;
};

Result<ParsedMetadata> ParseMetadata(std::span<const Byte> bytes, ui64 metadata_offset) {
  Cursor cursor(bytes);
  auto field_count = cursor.ReadU32();
  auto group_count = cursor.ReadU32();
  auto total_rows = cursor.ReadU64();
  if (!field_count || !group_count || !total_rows) {
    return MakeUnexpected(InvalidFormat("truncated metadata header"));
  }
  if (*field_count == 0 || *field_count > kColumnarMaxFieldCount) {
    return MakeUnexpected(InvalidFormat("invalid metadata field count"));
  }
  if (*group_count > kColumnarMaxRowGroupCount) {
    return MakeUnexpected(InvalidFormat("metadata has too many row groups"));
  }

  std::vector<Field> fields;
  fields.reserve(*field_count);
  for (ui32 index = 0; index < *field_count; ++index) {
    auto name_size = cursor.ReadU32();
    if (!name_size || *name_size == 0 || *name_size > kColumnarMaxFieldNameSize) {
      return MakeUnexpected(InvalidFormat("invalid field name size"));
    }
    auto name_bytes = cursor.ReadBytes(*name_size);
    auto type = cursor.ReadU8();
    auto nullable = cursor.ReadU8();
    auto flags = cursor.ReadU16();
    if (!name_bytes || !type || !nullable || !flags) {
      return MakeUnexpected(InvalidFormat("truncated field metadata"));
    }
    if (*type != static_cast<ui8>(LogicalType::kInt32) && *type != static_cast<ui8>(LogicalType::kString)) {
      return MakeUnexpected(InvalidFormat("unknown serialized logical type"));
    }
    if (*nullable > 1 || *flags != 0) {
      return MakeUnexpected(InvalidFormat("invalid field metadata flags"));
    }
    std::string name;
    name.reserve(*name_size);
    for (const Byte byte : *name_bytes) {
      name.push_back(static_cast<char>(std::to_integer<ui8>(byte)));
    }
    fields.push_back(
        Field{.name = std::move(name), .type = static_cast<LogicalType>(*type), .nullable = *nullable != 0});
  }

  auto schema = Schema::Create(std::move(fields));
  if (!schema) {
    return MakeUnexpected(InvalidFormat("invalid serialized schema: " + schema.error().Message()));
  }

  std::vector<RowGroupMetadata> row_groups;
  row_groups.reserve(*group_count);
  Index expected_first_row = 0;
  ui64 previous_chunk_end = kColumnarHeaderSize;
  for (ui32 group_index = 0; group_index < *group_count; ++group_index) {
    auto first_row = cursor.ReadU64();
    auto row_count = cursor.ReadU32();
    auto chunk_count = cursor.ReadU32();
    if (!first_row || !row_count || !chunk_count) {
      return MakeUnexpected(InvalidFormat("truncated row-group metadata"));
    }
    if (*row_count == 0) {
      return MakeUnexpected(InvalidFormat("empty row group is not allowed"));
    }
    if (*first_row != expected_first_row) {
      return MakeUnexpected(InvalidFormat("row groups are not contiguous"));
    }
    if (*chunk_count != *field_count) {
      return MakeUnexpected(InvalidFormat("row-group chunk count mismatch"));
    }
    if (*row_count > std::numeric_limits<Index>::max() - expected_first_row) {
      return MakeUnexpected(InvalidFormat("total row count overflow"));
    }

    RowGroupMetadata group;
    group.first_row = *first_row;
    group.row_count = *row_count;
    group.chunks.reserve(*chunk_count);
    for (ui32 column_index = 0; column_index < *chunk_count; ++column_index) {
      auto field = schema->FieldAt(column_index);
      if (!field) {
        return MakeUnexpected(std::move(field.error()));
      }
      auto offset = cursor.ReadU64();
      auto stored_size = cursor.ReadU64();
      auto decoded_size = cursor.ReadU64();
      auto null_count = cursor.ReadU32();
      auto encoding = cursor.ReadU8();
      auto compression = cursor.ReadU8();
      auto flags = cursor.ReadU16();
      if (!offset || !stored_size || !decoded_size || !null_count || !encoding || !compression || !flags) {
        return MakeUnexpected(InvalidFormat("truncated chunk metadata"));
      }
      if (*null_count > *row_count || (!field->get().nullable && *null_count != 0)) {
        return MakeUnexpected(InvalidFormat("invalid chunk null count"));
      }
      if (*flags != 0) {
        return MakeUnexpected(InvalidFormat("unsupported chunk flags"));
      }
      if (*encoding != static_cast<ui8>(ColumnEncoding::kPlain)) {
        return MakeUnexpected(InvalidFormat("unknown column encoding"));
      }
      if (*compression != static_cast<ui8>(ColumnCompression::kNone) &&
          *compression != static_cast<ui8>(ColumnCompression::kZstd)) {
        return MakeUnexpected(InvalidFormat("unknown column compression"));
      }
      if (*stored_size > kColumnarMaxChunkSize || *decoded_size > kColumnarMaxChunkSize) {
        return MakeUnexpected(InvalidFormat("invalid chunk size"));
      }
      if (*compression == static_cast<ui8>(ColumnCompression::kNone) && *stored_size != *decoded_size) {
        return MakeUnexpected(InvalidFormat("uncompressed chunk size mismatch"));
      }

      const ui64 bitmap_size = *null_count == 0 ? 0 : ValidityBitmapSize(*row_count);
      ui64 minimum_size = bitmap_size;
      const ui64 value_count =
          field->get().type == LogicalType::kString ? static_cast<ui64>(*row_count) + 1U : *row_count;
      const ui64 value_width = sizeof(ui32);
      if (value_count > (std::numeric_limits<ui64>::max() - minimum_size) / value_width ||
          !CheckedAdd(minimum_size, value_count * value_width, minimum_size)) {
        return MakeUnexpected(InvalidFormat("decoded chunk size overflow"));
      }
      if ((field->get().type == LogicalType::kInt32 && *decoded_size != minimum_size) ||
          (field->get().type == LogicalType::kString && *decoded_size < minimum_size)) {
        return MakeUnexpected(InvalidFormat("decoded chunk size mismatch"));
      }

      ui64 chunk_end = 0;
      if (*offset < previous_chunk_end || !CheckedAdd(*offset, *stored_size, chunk_end) ||
          chunk_end > metadata_offset) {
        return MakeUnexpected(InvalidFormat("chunk range is invalid"));
      }
      previous_chunk_end = chunk_end;
      group.chunks.push_back(ColumnChunkMetadata{.offset = *offset,
                                                 .stored_size = *stored_size,
                                                 .decoded_size = *decoded_size,
                                                 .null_count = *null_count,
                                                 .encoding = static_cast<ColumnEncoding>(*encoding),
                                                 .compression = static_cast<ColumnCompression>(*compression),
                                                 .flags = *flags});
    }
    row_groups.push_back(std::move(group));
    expected_first_row += *row_count;
  }

  if (expected_first_row != *total_rows) {
    return MakeUnexpected(InvalidFormat("metadata total row count mismatch"));
  }
  if (!cursor.Empty()) {
    return MakeUnexpected(InvalidFormat("metadata has trailing bytes"));
  }
  return ParsedMetadata{.schema = std::move(*schema), .row_groups = std::move(row_groups), .total_rows = *total_rows};
}

}  // namespace

ColumnarReader::ColumnarReader(std::unique_ptr<io::RandomAccessFile> file, Schema schema,
                               std::vector<RowGroupMetadata> row_groups, Index total_rows,
                               std::shared_ptr<const ColumnFactory> column_factory) noexcept
    : file_(std::move(file)),
      schema_(std::move(schema)),
      row_groups_(std::move(row_groups)),
      total_rows_(total_rows),
      column_factory_(std::move(column_factory)) {
}

Result<ColumnarReader> ColumnarReader::Open(std::unique_ptr<io::RandomAccessFile> file,
                                            std::shared_ptr<const ColumnFactory> column_factory) {
  if (file == nullptr) {
    return MakeUnexpected(Error(ErrorCode::kInvalidArgument, "input file must not be null"));
  }
  if (column_factory == nullptr) {
    return MakeUnexpected(Error(ErrorCode::kInvalidArgument, "column factory must not be null"));
  }
  auto size = file->Size();
  if (!size) {
    return MakeUnexpected(std::move(size.error()));
  }
  if (*size < kColumnarHeaderSize + kColumnarTrailerSize) {
    return MakeUnexpected(InvalidFormat("columnar file is too small"));
  }

  auto header = ReadRegion(*file, 0, kColumnarHeaderSize);
  if (!header) {
    return MakeUnexpected(std::move(header.error()));
  }
  auto header_preamble = ParsePreamble(*header, kColumnarHeaderMagic, "header");
  if (!header_preamble) {
    return MakeUnexpected(std::move(header_preamble.error()));
  }

  const ui64 trailer_offset = *size - kColumnarTrailerSize;
  auto trailer = ReadRegion(*file, trailer_offset, kColumnarTrailerSize);
  if (!trailer) {
    return MakeUnexpected(std::move(trailer.error()));
  }
  auto trailer_preamble =
      ParsePreamble(std::span<const Byte>(*trailer).first(kColumnarHeaderSize), kColumnarTrailerMagic, "trailer");
  if (!trailer_preamble) {
    return MakeUnexpected(std::move(trailer_preamble.error()));
  }
  if (header_preamble->major != trailer_preamble->major || header_preamble->minor != trailer_preamble->minor ||
      header_preamble->flags != trailer_preamble->flags) {
    return MakeUnexpected(InvalidFormat("header and trailer preambles disagree"));
  }

  Cursor trailer_cursor(*trailer);
  auto trailer_magic = trailer_cursor.ReadBytes(8);
  auto major = trailer_cursor.ReadU16();
  auto minor = trailer_cursor.ReadU16();
  auto flags = trailer_cursor.ReadU32();
  auto metadata_offset = trailer_cursor.ReadU64();
  auto metadata_size = trailer_cursor.ReadU64();
  auto encoded_file_size = trailer_cursor.ReadU64();
  if (!trailer_magic || !major || !minor || !flags || !metadata_offset || !metadata_size || !encoded_file_size) {
    return MakeUnexpected(InvalidFormat("truncated columnar trailer"));
  }
  if (*encoded_file_size != *size) {
    return MakeUnexpected(InvalidFormat("trailer file size mismatch"));
  }
  if (*metadata_size > kColumnarMaxMetadataSize || *metadata_offset < kColumnarHeaderSize) {
    return MakeUnexpected(InvalidFormat("invalid metadata range"));
  }
  ui64 metadata_end = 0;
  if (!CheckedAdd(*metadata_offset, *metadata_size, metadata_end) || metadata_end != trailer_offset) {
    return MakeUnexpected(InvalidFormat("invalid metadata range"));
  }

  auto metadata = ReadRegion(*file, *metadata_offset, *metadata_size);
  if (!metadata) {
    return MakeUnexpected(std::move(metadata.error()));
  }
  auto parsed = ParseMetadata(*metadata, *metadata_offset);
  if (!parsed) {
    return MakeUnexpected(std::move(parsed.error()));
  }
  return ColumnarReader(std::move(file), std::move(parsed->schema), std::move(parsed->row_groups), parsed->total_rows,
                        std::move(column_factory));
}

const Schema& ColumnarReader::GetSchema() const noexcept {
  return schema_;
}

Index ColumnarReader::RowCount() const noexcept {
  return total_rows_;
}

Index ColumnarReader::RowGroupCount() const noexcept {
  return row_groups_.size();
}

Result<DataChunk> ColumnarReader::ReadRowGroup(Index index) const {
  if (index >= row_groups_.size()) {
    return MakeUnexpected(Error(ErrorCode::kOutOfRange, "row-group index is out of range"));
  }
  const RowGroupMetadata& group = row_groups_[index];
  std::vector<ColumnVector> columns;
  columns.reserve(schema_.Size());

  for (Index column_index = 0; column_index < schema_.Size(); ++column_index) {
    const ColumnChunkMetadata& chunk = group.chunks[column_index];
    auto payload = ReadRegion(*file_, chunk.offset, chunk.stored_size);
    if (!payload) {
      return MakeUnexpected(std::move(payload.error()));
    }
    auto field = schema_.FieldAt(column_index);
    if (!field) {
      return MakeUnexpected(std::move(field.error()));
    }
    auto column = column_factory_->Create(*payload, field->get(), group.row_count,
                                          ColumnDecodeOptions{.decoded_size = chunk.decoded_size,
                                                              .null_count = chunk.null_count,
                                                              .encoding = chunk.encoding,
                                                              .compression = chunk.compression});
    if (!column) {
      return MakeUnexpected(std::move(column.error()));
    }
    if (column->Type() != field->get().type || column->Size() != group.row_count ||
        column->Validity().NullCount() != chunk.null_count) {
      return MakeUnexpected(InvalidFormat("column factory returned data that disagrees with chunk metadata"));
    }
    columns.push_back(std::move(*column));
  }

  auto result = DataChunk::Create(schema_, std::move(columns));
  if (!result) {
    return MakeUnexpected(InvalidFormat(result.error().Message()));
  }
  return result;
}

Result<std::optional<DataChunk>> ColumnarReader::Next() {
  if (next_row_group_ == row_groups_.size()) {
    return std::optional<DataChunk>();
  }
  auto chunk = ReadRowGroup(next_row_group_);
  if (!chunk) {
    return MakeUnexpected(std::move(chunk.error()));
  }
  ++next_row_group_;
  return std::optional<DataChunk>(std::move(*chunk));
}

void ColumnarReader::Reset() noexcept {
  next_row_group_ = 0;
}

}  // namespace borophene::execution
