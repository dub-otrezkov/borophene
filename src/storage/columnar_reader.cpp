#include "borophene/storage/columnar_reader.hpp"

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

Error InvalidFormat(std::string message) {
  return {ErrorCode::kInvalidFormat, std::move(message)};
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

class Cursor {
 public:
  explicit Cursor(std::span<const std::byte> bytes) : bytes_(bytes) {
  }

  Result<std::uint8_t> ReadU8() {
    if (position_ == bytes_.size()) {
      return std::unexpected(InvalidFormat("truncated columnar metadata"));
    }
    return std::to_integer<std::uint8_t>(bytes_[position_++]);
  }

  Result<std::uint16_t> ReadU16() {
    auto bytes = ReadBytes(sizeof(std::uint16_t));
    if (!bytes) {
      return std::unexpected(std::move(bytes.error()));
    }
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*bytes)[0]) |
                                      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*bytes)[1])) << 8U));
  }

  Result<std::uint32_t> ReadU32() {
    auto bytes = ReadBytes(sizeof(std::uint32_t));
    if (!bytes) {
      return std::unexpected(std::move(bytes.error()));
    }
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*bytes)[index])) << (index * 8U);
    }
    return value;
  }

  Result<std::uint64_t> ReadU64() {
    auto bytes = ReadBytes(sizeof(std::uint64_t));
    if (!bytes) {
      return std::unexpected(std::move(bytes.error()));
    }
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>((*bytes)[index])) << (index * 8U);
    }
    return value;
  }

  Result<std::span<const std::byte>> ReadBytes(std::uint64_t size) {
    if (size > bytes_.size() - position_) {
      return std::unexpected(InvalidFormat("truncated columnar metadata"));
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
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
};

bool MatchesMagic(std::span<const std::byte> bytes, const std::array<char, 8>& magic) {
  if (bytes.size() != magic.size()) {
    return false;
  }
  for (std::size_t index = 0; index < magic.size(); ++index) {
    if (std::to_integer<std::uint8_t>(bytes[index]) != static_cast<std::uint8_t>(magic[index])) {
      return false;
    }
  }
  return true;
}

Result<Buffer> ReadRegion(const io::RandomAccessFile& file, std::uint64_t offset, std::uint64_t size) {
  if (size > std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(InvalidFormat("columnar region is too large"));
  }
  Buffer bytes(static_cast<std::size_t>(size));
  auto read = io::ReadExactly(file, offset, bytes);
  if (!read) {
    return std::unexpected(std::move(read.error()));
  }
  return bytes;
}

struct Preamble {
  std::uint16_t major;
  std::uint16_t minor;
  std::uint32_t flags;
};

Result<Preamble> ParsePreamble(std::span<const std::byte> bytes, const std::array<char, 8>& magic,
                               std::string_view location) {
  Cursor cursor(bytes);
  auto encoded_magic = cursor.ReadBytes(magic.size());
  if (!encoded_magic || !MatchesMagic(*encoded_magic, magic)) {
    return std::unexpected(InvalidFormat(std::string(location) + " magic is invalid"));
  }
  auto major = cursor.ReadU16();
  auto minor = cursor.ReadU16();
  auto flags = cursor.ReadU32();
  if (!major || !minor || !flags) {
    return std::unexpected(InvalidFormat(std::string(location) + " is truncated"));
  }
  if (*major != kColumnarFormatMajor || *minor != kColumnarFormatMinor) {
    return std::unexpected(
        Error(ErrorCode::kUnsupportedVersion,
              "unsupported columnar format version " + std::to_string(*major) + "." + std::to_string(*minor)));
  }
  if (*flags != kColumnarFormatFlags) {
    return std::unexpected(InvalidFormat(std::string(location) + " has unknown flags"));
  }
  return Preamble{.major = *major, .minor = *minor, .flags = *flags};
}

struct ParsedMetadata {
  Schema schema;
  std::vector<RowGroupMetadata> row_groups;
  Index total_rows;
};

Result<ParsedMetadata> ParseMetadata(std::span<const std::byte> bytes, std::uint64_t metadata_offset) {
  Cursor cursor(bytes);
  auto field_count = cursor.ReadU32();
  auto group_count = cursor.ReadU32();
  auto total_rows = cursor.ReadU64();
  if (!field_count || !group_count || !total_rows) {
    return std::unexpected(InvalidFormat("truncated metadata header"));
  }
  if (*field_count == 0 || *field_count > kColumnarMaxFieldCount) {
    return std::unexpected(InvalidFormat("invalid metadata field count"));
  }
  if (*group_count > kColumnarMaxRowGroupCount) {
    return std::unexpected(InvalidFormat("metadata has too many row groups"));
  }

  std::vector<Field> fields;
  fields.reserve(*field_count);
  for (std::uint32_t index = 0; index < *field_count; ++index) {
    auto name_size = cursor.ReadU32();
    if (!name_size || *name_size == 0 || *name_size > kColumnarMaxFieldNameSize) {
      return std::unexpected(InvalidFormat("invalid field name size"));
    }
    auto name_bytes = cursor.ReadBytes(*name_size);
    auto type = cursor.ReadU8();
    auto nullable = cursor.ReadU8();
    auto flags = cursor.ReadU16();
    if (!name_bytes || !type || !nullable || !flags) {
      return std::unexpected(InvalidFormat("truncated field metadata"));
    }
    if (*type != static_cast<std::uint8_t>(LogicalType::kInt32) &&
        *type != static_cast<std::uint8_t>(LogicalType::kString)) {
      return std::unexpected(InvalidFormat("unknown serialized logical type"));
    }
    if (*nullable > 1 || *flags != 0) {
      return std::unexpected(InvalidFormat("invalid field metadata flags"));
    }
    std::string name;
    name.reserve(*name_size);
    for (const std::byte byte : *name_bytes) {
      name.push_back(static_cast<char>(std::to_integer<std::uint8_t>(byte)));
    }
    fields.push_back(
        Field{.name = std::move(name), .type = static_cast<LogicalType>(*type), .nullable = *nullable != 0});
  }

  auto schema = Schema::Create(std::move(fields));
  if (!schema) {
    return std::unexpected(InvalidFormat("invalid serialized schema: " + schema.error().Message()));
  }

  std::vector<RowGroupMetadata> row_groups;
  row_groups.reserve(*group_count);
  Index expected_first_row = 0;
  std::uint64_t previous_chunk_end = kColumnarHeaderSize;
  for (std::uint32_t group_index = 0; group_index < *group_count; ++group_index) {
    auto first_row = cursor.ReadU64();
    auto row_count = cursor.ReadU32();
    auto chunk_count = cursor.ReadU32();
    if (!first_row || !row_count || !chunk_count) {
      return std::unexpected(InvalidFormat("truncated row-group metadata"));
    }
    if (*row_count == 0) {
      return std::unexpected(InvalidFormat("empty row group is not allowed"));
    }
    if (*first_row != expected_first_row) {
      return std::unexpected(InvalidFormat("row groups are not contiguous"));
    }
    if (*chunk_count != *field_count) {
      return std::unexpected(InvalidFormat("row-group chunk count mismatch"));
    }
    if (*row_count > std::numeric_limits<Index>::max() - expected_first_row) {
      return std::unexpected(InvalidFormat("total row count overflow"));
    }

    RowGroupMetadata group;
    group.first_row = *first_row;
    group.row_count = *row_count;
    group.chunks.reserve(*chunk_count);
    for (std::uint32_t column_index = 0; column_index < *chunk_count; ++column_index) {
      auto offset = cursor.ReadU64();
      auto stored_size = cursor.ReadU64();
      auto decoded_size = cursor.ReadU64();
      auto null_count = cursor.ReadU32();
      auto encoding = cursor.ReadU8();
      auto compression = cursor.ReadU8();
      auto flags = cursor.ReadU16();
      if (!offset || !stored_size || !decoded_size || !null_count || !encoding || !compression || !flags) {
        return std::unexpected(InvalidFormat("truncated chunk metadata"));
      }
      if (*null_count > *row_count || (!(*schema)[column_index].nullable && *null_count != 0)) {
        return std::unexpected(InvalidFormat("invalid chunk null count"));
      }
      if (*encoding != static_cast<std::uint8_t>(ColumnEncoding::kPlain) ||
          *compression != static_cast<std::uint8_t>(ColumnCompression::kNone) || *flags != 0) {
        return std::unexpected(InvalidFormat("unsupported chunk options"));
      }
      if (*stored_size != *decoded_size || *stored_size > kColumnarMaxChunkSize) {
        return std::unexpected(InvalidFormat("invalid chunk size"));
      }

      const std::uint64_t bitmap_size = *null_count == 0 ? 0 : ValidityBitmapSize(*row_count);
      std::uint64_t minimum_size = bitmap_size;
      const std::uint64_t value_count = (*schema)[column_index].type == LogicalType::kString
                                            ? static_cast<std::uint64_t>(*row_count) + 1U
                                            : *row_count;
      const std::uint64_t value_width = sizeof(std::uint32_t);
      if (value_count > (std::numeric_limits<std::uint64_t>::max() - minimum_size) / value_width ||
          !CheckedAdd(minimum_size, value_count * value_width, minimum_size)) {
        return std::unexpected(InvalidFormat("decoded chunk size overflow"));
      }
      if (((*schema)[column_index].type == LogicalType::kInt32 && *decoded_size != minimum_size) ||
          ((*schema)[column_index].type == LogicalType::kString && *decoded_size < minimum_size)) {
        return std::unexpected(InvalidFormat("decoded chunk size mismatch"));
      }

      std::uint64_t chunk_end = 0;
      if (*offset < previous_chunk_end || !CheckedAdd(*offset, *stored_size, chunk_end) ||
          chunk_end > metadata_offset) {
        return std::unexpected(InvalidFormat("chunk range is invalid"));
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
    return std::unexpected(InvalidFormat("metadata total row count mismatch"));
  }
  if (!cursor.Empty()) {
    return std::unexpected(InvalidFormat("metadata has trailing bytes"));
  }
  return ParsedMetadata{.schema = std::move(*schema), .row_groups = std::move(row_groups), .total_rows = *total_rows};
}

std::uint32_t DecodeU32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned int index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
  }
  return value;
}

Result<ValidityMask> DecodeValidity(std::span<const std::byte> payload, Index row_count,
                                    std::uint32_t expected_null_count) {
  ValidityMask validity(row_count);
  if (expected_null_count == 0) {
    return validity;
  }
  const std::uint64_t bitmap_size = ValidityBitmapSize(row_count);
  if (payload.size() < bitmap_size) {
    return std::unexpected(InvalidFormat("truncated validity bitmap"));
  }
  if (row_count % 8U != 0U) {
    const auto last = std::to_integer<std::uint8_t>(payload[static_cast<std::size_t>(bitmap_size - 1U)]);
    const auto used_mask = static_cast<std::uint8_t>((1U << (row_count % 8U)) - 1U);
    if ((last & static_cast<std::uint8_t>(~used_mask)) != 0) {
      return std::unexpected(InvalidFormat("validity bitmap padding bits are not zero"));
    }
  }
  for (Index row = 0; row < row_count; ++row) {
    const auto byte = std::to_integer<std::uint8_t>(payload[static_cast<std::size_t>(row / 8U)]);
    validity.SetValid(row, (byte & (1U << (row % 8U))) != 0);
  }
  if (validity.NullCount() != expected_null_count) {
    return std::unexpected(InvalidFormat("validity null count mismatch"));
  }
  return validity;
}

}  // namespace

ColumnarReader::ColumnarReader(std::unique_ptr<io::RandomAccessFile> file, Schema schema,
                               std::vector<RowGroupMetadata> row_groups, Index total_rows) noexcept
    : file_(std::move(file)), schema_(std::move(schema)), row_groups_(std::move(row_groups)), total_rows_(total_rows) {
}

Result<ColumnarReader> ColumnarReader::Open(std::unique_ptr<io::RandomAccessFile> file) {
  if (file == nullptr) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument, "input file must not be null"));
  }
  auto size = file->Size();
  if (!size) {
    return std::unexpected(std::move(size.error()));
  }
  if (*size < kColumnarHeaderSize + kColumnarTrailerSize) {
    return std::unexpected(InvalidFormat("columnar file is too small"));
  }

  auto header = ReadRegion(*file, 0, kColumnarHeaderSize);
  if (!header) {
    return std::unexpected(std::move(header.error()));
  }
  auto header_preamble = ParsePreamble(*header, kColumnarHeaderMagic, "header");
  if (!header_preamble) {
    return std::unexpected(std::move(header_preamble.error()));
  }

  const std::uint64_t trailer_offset = *size - kColumnarTrailerSize;
  auto trailer = ReadRegion(*file, trailer_offset, kColumnarTrailerSize);
  if (!trailer) {
    return std::unexpected(std::move(trailer.error()));
  }
  auto trailer_preamble =
      ParsePreamble(std::span<const std::byte>(*trailer).first(kColumnarHeaderSize), kColumnarTrailerMagic, "trailer");
  if (!trailer_preamble) {
    return std::unexpected(std::move(trailer_preamble.error()));
  }
  if (header_preamble->major != trailer_preamble->major || header_preamble->minor != trailer_preamble->minor ||
      header_preamble->flags != trailer_preamble->flags) {
    return std::unexpected(InvalidFormat("header and trailer preambles disagree"));
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
    return std::unexpected(InvalidFormat("truncated columnar trailer"));
  }
  if (*encoded_file_size != *size) {
    return std::unexpected(InvalidFormat("trailer file size mismatch"));
  }
  if (*metadata_size > kColumnarMaxMetadataSize || *metadata_offset < kColumnarHeaderSize) {
    return std::unexpected(InvalidFormat("invalid metadata range"));
  }
  std::uint64_t metadata_end = 0;
  if (!CheckedAdd(*metadata_offset, *metadata_size, metadata_end) || metadata_end != trailer_offset) {
    return std::unexpected(InvalidFormat("invalid metadata range"));
  }

  auto metadata = ReadRegion(*file, *metadata_offset, *metadata_size);
  if (!metadata) {
    return std::unexpected(std::move(metadata.error()));
  }
  auto parsed = ParseMetadata(*metadata, *metadata_offset);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return ColumnarReader(std::move(file), std::move(parsed->schema), std::move(parsed->row_groups), parsed->total_rows);
}

Result<ColumnarReader> ColumnarReader::Open(const std::filesystem::path& path) {
  auto file = io::OpenLocalInput(path);
  if (!file) {
    return std::unexpected(std::move(file.error()));
  }
  return Open(std::move(*file));
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
    return std::unexpected(Error(ErrorCode::kOutOfRange, "row-group index is out of range"));
  }
  const RowGroupMetadata& group = row_groups_[index];
  std::vector<ColumnVector> columns;
  columns.reserve(schema_.Size());

  for (Index column_index = 0; column_index < schema_.Size(); ++column_index) {
    const ColumnChunkMetadata& chunk = group.chunks[column_index];
    auto payload = ReadRegion(*file_, chunk.offset, chunk.stored_size);
    if (!payload) {
      return std::unexpected(std::move(payload.error()));
    }
    auto validity = DecodeValidity(*payload, group.row_count, chunk.null_count);
    if (!validity) {
      return std::unexpected(std::move(validity.error()));
    }
    const auto bitmap_size = static_cast<std::size_t>(chunk.null_count == 0 ? 0 : ValidityBitmapSize(group.row_count));

    if (schema_[column_index].type == LogicalType::kInt32) {
      std::vector<std::int32_t> values;
      values.reserve(group.row_count);
      for (Index row = 0; row < group.row_count; ++row) {
        const std::uint32_t encoded = DecodeU32(*payload, bitmap_size + static_cast<std::size_t>(row) * 4U);
        const auto value = std::bit_cast<std::int32_t>(encoded);
        if (!validity->IsValid(row) && value != 0) {
          return std::unexpected(InvalidFormat("null int32 value is not encoded as zero"));
        }
        values.push_back(value);
      }
      auto column = ColumnVector::CreateInt32(std::move(values), std::move(*validity));
      if (!column) {
        return std::unexpected(InvalidFormat(column.error().Message()));
      }
      columns.push_back(std::move(*column));
      continue;
    }

    const std::size_t offset_table_size = (static_cast<std::size_t>(group.row_count) + 1U) * 4U;
    const std::size_t data_begin = bitmap_size + offset_table_size;
    const std::size_t string_data_size = payload->size() - data_begin;
    std::vector<std::uint32_t> offsets;
    offsets.reserve(static_cast<std::size_t>(group.row_count) + 1U);
    for (Index row = 0; row <= group.row_count; ++row) {
      offsets.push_back(DecodeU32(*payload, bitmap_size + static_cast<std::size_t>(row) * 4U));
    }
    if (offsets.front() != 0 || offsets.back() != string_data_size) {
      return std::unexpected(InvalidFormat("invalid string offset bounds"));
    }

    std::vector<std::string> values;
    values.reserve(group.row_count);
    for (Index row = 0; row < group.row_count; ++row) {
      const std::uint32_t begin = offsets[static_cast<std::size_t>(row)];
      const std::uint32_t end = offsets[static_cast<std::size_t>(row) + 1U];
      if (begin > end || end > string_data_size) {
        return std::unexpected(InvalidFormat("invalid string offsets"));
      }
      if (!validity->IsValid(row) && begin != end) {
        return std::unexpected(InvalidFormat("null string has a non-empty byte range"));
      }
      const char* data = reinterpret_cast<const char*>(payload->data()) + data_begin + begin;
      values.emplace_back(data, end - begin);
    }
    auto column = ColumnVector::CreateString(std::move(values), std::move(*validity));
    if (!column) {
      return std::unexpected(InvalidFormat(column.error().Message()));
    }
    columns.push_back(std::move(*column));
  }

  auto result = DataChunk::Create(schema_, std::move(columns));
  if (!result) {
    return std::unexpected(InvalidFormat(result.error().Message()));
  }
  return result;
}

Result<std::optional<DataChunk>> ColumnarReader::Next() {
  if (next_row_group_ == row_groups_.size()) {
    return std::optional<DataChunk>();
  }
  auto chunk = ReadRowGroup(next_row_group_);
  if (!chunk) {
    return std::unexpected(std::move(chunk.error()));
  }
  ++next_row_group_;
  return std::optional<DataChunk>(std::move(*chunk));
}

void ColumnarReader::Reset() noexcept {
  next_row_group_ = 0;
}

}  // namespace borophene::storage
