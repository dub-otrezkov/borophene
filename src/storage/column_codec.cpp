#include "borophene/storage/column_codec.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace borophene::storage {
namespace {

using Buffer = std::vector<Byte>;

void AppendU8(Buffer& output, ui8 value) {
  output.push_back(static_cast<Byte>(value));
}

void AppendU32(Buffer& output, ui32 value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    AppendU8(output, static_cast<ui8>(value >> shift));
  }
}

Result<bool> IsValid(const ValidityMask& validity, Index row) {
  auto valid = validity.IsValid(row);
  if (!valid) {
    return MakeUnexpected(std::move(valid.error()));
  }
  return *valid;
}

Result<ui64> PlainEncodedSize(const ColumnVector& column) {
  ui64 size = column.Validity().NullCount() == 0 ? 0 : ValidityBitmapSize(column.Size());
  if (size > kColumnarMaxChunkSize) {
    return Failure<ui64>(ErrorCode::kInvalidArgument, "validity bitmap exceeds the format limit");
  }
  if (column.Type() == LogicalType::kInt32) {
    auto values = column.Int32Values();
    if (!values) {
      return MakeUnexpected(std::move(values.error()));
    }
    if (values->get().size() != column.Size() || column.Size() > (kColumnarMaxChunkSize - size) / sizeof(i32)) {
      return Failure<ui64>(ErrorCode::kInvalidArgument, "int32 column exceeds the format limit");
    }
    return size + column.Size() * sizeof(i32);
  }
  if (column.Type() != LogicalType::kString) {
    return Failure<ui64>(ErrorCode::kInvalidArgument, "unsupported logical type");
  }

  auto values = column.StringValues();
  if (!values) {
    return MakeUnexpected(std::move(values.error()));
  }
  const auto& data = values->get();
  if (data.size() != column.Size() || column.Size() == std::numeric_limits<ui32>::max()) {
    return Failure<ui64>(ErrorCode::kInvalidArgument, "string column exceeds the format limit");
  }
  const ui64 offset_count = column.Size() + 1U;
  if (offset_count > (kColumnarMaxChunkSize - size) / sizeof(ui32)) {
    return Failure<ui64>(ErrorCode::kInvalidArgument, "string offsets exceed the format limit");
  }
  size += offset_count * sizeof(ui32);

  ui64 string_bytes = 0;
  for (Index row = 0; row < column.Size(); ++row) {
    auto valid = IsValid(column.Validity(), row);
    if (!valid) {
      return MakeUnexpected(std::move(valid.error()));
    }
    if (!*valid) {
      continue;
    }
    const auto& value = data[static_cast<std::size_t>(row)];
    if (value.size() > std::numeric_limits<ui32>::max() - string_bytes ||
        value.size() > kColumnarMaxChunkSize - size - string_bytes) {
      return Failure<ui64>(ErrorCode::kInvalidArgument, "string data exceeds the format limit");
    }
    string_bytes += value.size();
  }
  return size + string_bytes;
}

Result<Buffer> EncodeValidity(const ColumnVector& column) {
  Buffer output;
  const ValidityMask& validity = column.Validity();
  if (validity.NullCount() == 0) {
    return output;
  }

  const ui64 bitmap_size = ValidityBitmapSize(column.Size());
  if (bitmap_size > output.max_size()) {
    return Failure<Buffer>(ErrorCode::kInvalidArgument, "validity bitmap is too large");
  }
  output.resize(static_cast<std::size_t>(bitmap_size));
  for (Index row = 0; row < column.Size(); ++row) {
    auto valid = IsValid(validity, row);
    if (!valid) {
      return MakeUnexpected(std::move(valid.error()));
    }
    if (*valid) {
      const auto bit = static_cast<ui8>(1U << (row % 8U));
      output[static_cast<std::size_t>(row / 8U)] |= static_cast<Byte>(bit);
    }
  }
  return output;
}

Result<Buffer> EncodeInt32(const ColumnVector& column, Buffer output) {
  auto values = column.Int32Values();
  if (!values) {
    return MakeUnexpected(std::move(values.error()));
  }
  const auto& data = values->get();
  if (data.size() != column.Size() || column.Size() > (output.max_size() - output.size()) / sizeof(i32)) {
    return Failure<Buffer>(ErrorCode::kInvalidArgument, "int32 column is too large");
  }

  output.reserve(output.size() + data.size() * sizeof(i32));
  for (Index row = 0; row < column.Size(); ++row) {
    auto valid = IsValid(column.Validity(), row);
    if (!valid) {
      return MakeUnexpected(std::move(valid.error()));
    }
    const i32 value = *valid ? data[static_cast<std::size_t>(row)] : 0;
    AppendU32(output, std::bit_cast<ui32>(value));
  }
  return output;
}

Result<Buffer> EncodeString(const ColumnVector& column, Buffer output) {
  auto values = column.StringValues();
  if (!values) {
    return MakeUnexpected(std::move(values.error()));
  }
  const auto& data = values->get();
  if (data.size() != column.Size() || column.Size() == std::numeric_limits<ui32>::max()) {
    return Failure<Buffer>(ErrorCode::kInvalidArgument, "string column is too large");
  }

  const ui64 offset_count = column.Size() + 1U;
  if (offset_count > (output.max_size() - output.size()) / sizeof(ui32)) {
    return Failure<Buffer>(ErrorCode::kInvalidArgument, "string offset table is too large");
  }

  std::vector<ui32> offsets;
  offsets.reserve(static_cast<std::size_t>(offset_count));
  offsets.push_back(0);
  ui64 byte_count = 0;
  for (Index row = 0; row < column.Size(); ++row) {
    auto valid = IsValid(column.Validity(), row);
    if (!valid) {
      return MakeUnexpected(std::move(valid.error()));
    }
    if (*valid) {
      const auto& value = data[static_cast<std::size_t>(row)];
      if (value.size() > std::numeric_limits<ui32>::max() - byte_count) {
        return Failure<Buffer>(ErrorCode::kInvalidArgument, "string data exceeds the format limit");
      }
      byte_count += value.size();
    }
    offsets.push_back(static_cast<ui32>(byte_count));
  }

  const ui64 offset_bytes = offset_count * sizeof(ui32);
  if (byte_count > output.max_size() - output.size() - offset_bytes) {
    return Failure<Buffer>(ErrorCode::kInvalidArgument, "string column is too large");
  }
  output.reserve(output.size() + static_cast<std::size_t>(offset_bytes + byte_count));
  for (const ui32 offset : offsets) {
    AppendU32(output, offset);
  }
  for (Index row = 0; row < column.Size(); ++row) {
    auto valid = IsValid(column.Validity(), row);
    if (!valid) {
      return MakeUnexpected(std::move(valid.error()));
    }
    if (!*valid) {
      continue;
    }
    for (const char byte : data[static_cast<std::size_t>(row)]) {
      AppendU8(output, static_cast<ui8>(byte));
    }
  }
  return output;
}

Result<Buffer> EncodePlain(const ColumnVector& column) {
  auto output = EncodeValidity(column);
  if (!output) {
    return MakeUnexpected(std::move(output.error()));
  }
  if (column.Type() == LogicalType::kInt32) {
    return EncodeInt32(column, std::move(*output));
  }
  if (column.Type() == LogicalType::kString) {
    return EncodeString(column, std::move(*output));
  }
  return Failure<Buffer>(ErrorCode::kInvalidArgument, "unsupported logical type");
}

ui32 DecodeU32(std::span<const Byte> bytes, std::size_t offset) {
  ui32 value = 0;
  for (unsigned int index = 0; index < 4U; ++index) {
    value |= static_cast<ui32>(std::to_integer<ui8>(bytes[offset + index])) << (index * 8U);
  }
  return value;
}

Result<ValidityMask> DecodeValidity(std::span<const Byte> payload, Index row_count, ui32 expected_null_count) {
  if (expected_null_count > row_count) {
    return Failure<ValidityMask>(ErrorCode::kInvalidFormat, "invalid chunk null count");
  }
  auto validity = ValidityMask::Create(row_count);
  if (!validity) {
    return MakeUnexpected(std::move(validity.error()));
  }
  if (expected_null_count == 0) {
    return validity;
  }

  const ui64 bitmap_size = ValidityBitmapSize(row_count);
  if (bitmap_size > payload.size()) {
    return Failure<ValidityMask>(ErrorCode::kInvalidFormat, "truncated validity bitmap");
  }
  if (row_count % 8U != 0U) {
    const auto last = std::to_integer<ui8>(payload[static_cast<std::size_t>(bitmap_size - 1U)]);
    const auto used_mask = static_cast<ui8>((1U << (row_count % 8U)) - 1U);
    if ((last & static_cast<ui8>(~used_mask)) != 0) {
      return Failure<ValidityMask>(ErrorCode::kInvalidFormat, "validity bitmap padding bits are not zero");
    }
  }
  for (Index row = 0; row < row_count; ++row) {
    const auto byte = std::to_integer<ui8>(payload[static_cast<std::size_t>(row / 8U)]);
    auto set_valid = validity->SetValid(row, (byte & (1U << (row % 8U))) != 0);
    if (!set_valid) {
      return MakeUnexpected(std::move(set_valid.error()));
    }
  }
  if (validity->NullCount() != expected_null_count) {
    return Failure<ValidityMask>(ErrorCode::kInvalidFormat, "validity null count mismatch");
  }
  return validity;
}

Result<ColumnVector> DecodeInt32(std::span<const Byte> payload, Index row_count, ValidityMask validity) {
  const auto bitmap_size = static_cast<std::size_t>(validity.NullCount() == 0 ? 0 : ValidityBitmapSize(row_count));
  if (row_count > (std::numeric_limits<std::size_t>::max() - bitmap_size) / sizeof(ui32) ||
      payload.size() != bitmap_size + static_cast<std::size_t>(row_count) * sizeof(ui32)) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "decoded int32 chunk size mismatch");
  }

  std::vector<i32> values;
  values.reserve(static_cast<std::size_t>(row_count));
  for (Index row = 0; row < row_count; ++row) {
    const ui32 encoded = DecodeU32(payload, bitmap_size + static_cast<std::size_t>(row) * sizeof(ui32));
    const i32 value = std::bit_cast<i32>(encoded);
    auto valid = validity.IsValid(row);
    if (!valid) {
      return MakeUnexpected(std::move(valid.error()));
    }
    if (!*valid && value != 0) {
      return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "null int32 value is not encoded as zero");
    }
    values.push_back(value);
  }

  auto column = ColumnVector::CreateInt32(std::move(values), std::optional<ValidityMask>(std::move(validity)));
  if (!column) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, column.error().Message());
  }
  return column;
}

Result<ColumnVector> DecodeString(std::span<const Byte> payload, Index row_count, ValidityMask validity) {
  const auto bitmap_size = static_cast<std::size_t>(validity.NullCount() == 0 ? 0 : ValidityBitmapSize(row_count));
  if (row_count == std::numeric_limits<Index>::max() ||
      row_count + 1U > (std::numeric_limits<std::size_t>::max() - bitmap_size) / sizeof(ui32)) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "decoded string offset table is too large");
  }
  const std::size_t offset_table_size = static_cast<std::size_t>(row_count + 1U) * sizeof(ui32);
  if (offset_table_size > payload.size() - bitmap_size) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "truncated string offset table");
  }
  const std::size_t data_begin = bitmap_size + offset_table_size;
  const std::size_t string_data_size = payload.size() - data_begin;

  std::vector<ui32> offsets;
  offsets.reserve(static_cast<std::size_t>(row_count) + 1U);
  for (Index row = 0; row <= row_count; ++row) {
    offsets.push_back(DecodeU32(payload, bitmap_size + static_cast<std::size_t>(row) * sizeof(ui32)));
  }
  if (offsets.front() != 0 || offsets.back() != string_data_size) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "invalid string offset bounds");
  }

  std::vector<std::string> values;
  values.reserve(static_cast<std::size_t>(row_count));
  for (Index row = 0; row < row_count; ++row) {
    const ui32 begin = offsets[static_cast<std::size_t>(row)];
    const ui32 end = offsets[static_cast<std::size_t>(row) + 1U];
    if (begin > end || end > string_data_size) {
      return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "invalid string offsets");
    }
    auto valid = validity.IsValid(row);
    if (!valid) {
      return MakeUnexpected(std::move(valid.error()));
    }
    if (!*valid && begin != end) {
      return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "null string has a non-empty byte range");
    }
    const char* data = reinterpret_cast<const char*>(payload.data()) + data_begin + begin;
    values.emplace_back(data, end - begin);
  }

  auto column = ColumnVector::CreateString(std::move(values), std::optional<ValidityMask>(std::move(validity)));
  if (!column) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, column.error().Message());
  }
  return column;
}

Result<Buffer> CompressColumn(Buffer bytes, ColumnCompression compression) {
  if (compression != ColumnCompression::kNone) {
    return Failure<Buffer>(ErrorCode::kUnsupportedVersion, "unsupported column compression");
  }
  return bytes;
}

Result<SerializedColumn> SerializeColumn(const ColumnVector& column, const Field& field,
                                         const ColumnCodecOptions& options) {
  if (column.Type() != field.type) {
    return Failure<SerializedColumn>(ErrorCode::kSchemaMismatch,
                                     "row-group column type does not match the writer schema");
  }
  if (column.Validity().Size() != column.Size()) {
    return Failure<SerializedColumn>(ErrorCode::kInvalidArgument, "column validity size mismatch");
  }
  if (!field.nullable && column.Validity().NullCount() != 0) {
    return Failure<SerializedColumn>(ErrorCode::kSchemaMismatch, "a non-nullable column contains null values");
  }
  if (column.Validity().NullCount() > std::numeric_limits<ui32>::max()) {
    return Failure<SerializedColumn>(ErrorCode::kInvalidArgument, "column has too many null values");
  }
  if (options.encoding != ColumnEncoding::kPlain) {
    return Failure<SerializedColumn>(ErrorCode::kUnsupportedVersion, "unsupported column encoding");
  }
  if (options.compression != ColumnCompression::kNone) {
    return Failure<SerializedColumn>(ErrorCode::kUnsupportedVersion, "unsupported column compression");
  }

  auto expected_size = PlainEncodedSize(column);
  if (!expected_size) {
    return MakeUnexpected(std::move(expected_size.error()));
  }

  auto encoded = EncodePlain(column);
  if (!encoded) {
    return MakeUnexpected(std::move(encoded.error()));
  }
  if (encoded->size() > kColumnarMaxChunkSize) {
    return Failure<SerializedColumn>(ErrorCode::kInvalidArgument, "encoded column chunk exceeds the format limit");
  }
  if (encoded->size() != *expected_size) {
    return Failure<SerializedColumn>(ErrorCode::kInvalidState,
                                     "plain column size preflight disagrees with serialized bytes");
  }
  const ui64 decoded_size = encoded->size();
  auto compressed = CompressColumn(std::move(*encoded), options.compression);
  if (!compressed) {
    return MakeUnexpected(std::move(compressed.error()));
  }
  if (compressed->size() > kColumnarMaxChunkSize) {
    return Failure<SerializedColumn>(ErrorCode::kInvalidArgument, "compressed column chunk exceeds the format limit");
  }

  return SerializedColumn{.bytes = std::move(*compressed),
                          .decoded_size = decoded_size,
                          .null_count = static_cast<ui32>(column.Validity().NullCount()),
                          .encoding = options.encoding,
                          .compression = options.compression};
}

Result<ColumnVector> DeserializeColumn(std::span<const Byte> bytes, const Field& field, Index row_count,
                                       const ColumnDecodeOptions& options) {
  if (row_count > std::numeric_limits<ui32>::max()) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "row count exceeds the format limit");
  }
  if (bytes.size() > kColumnarMaxChunkSize || options.decoded_size > kColumnarMaxChunkSize) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "column chunk exceeds the format limit");
  }
  if (options.encoding != ColumnEncoding::kPlain) {
    return Failure<ColumnVector>(ErrorCode::kUnsupportedVersion, "unsupported column encoding");
  }
  if (!field.nullable && options.null_count != 0) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "non-nullable column contains null values");
  }

  if (options.compression != ColumnCompression::kNone) {
    return Failure<ColumnVector>(ErrorCode::kUnsupportedVersion, "unsupported column compression");
  }
  if (bytes.size() != options.decoded_size) {
    return Failure<ColumnVector>(ErrorCode::kInvalidFormat, "uncompressed column size mismatch");
  }
  auto validity = DecodeValidity(bytes, row_count, options.null_count);
  if (!validity) {
    return MakeUnexpected(std::move(validity.error()));
  }
  if (field.type == LogicalType::kInt32) {
    return DecodeInt32(bytes, row_count, std::move(*validity));
  }
  if (field.type == LogicalType::kString) {
    return DecodeString(bytes, row_count, std::move(*validity));
  }
  return Failure<ColumnVector>(ErrorCode::kUnsupportedVersion, "unsupported logical type");
}

class V1ColumnEncoder final : public ColumnEncoder {
 public:
  explicit V1ColumnEncoder(ColumnCodecOptions options) : options_(options) {
  }

  Result<SerializedColumn> Serialize(const ColumnVector& column, const Field& field) const override {
    return SerializeColumn(column, field, options_);
  }

 private:
  ColumnCodecOptions options_;
};

class V1ColumnFactory final : public ColumnFactory {
 public:
  Result<ColumnVector> Create(std::span<const Byte> bytes, const Field& field, Index row_count,
                              const ColumnDecodeOptions& options) const override {
    return DeserializeColumn(bytes, field, row_count, options);
  }
};

}  // namespace

std::shared_ptr<const ColumnEncoder> CreateV1ColumnEncoder(const ColumnCodecOptions& options) {
  return std::make_shared<V1ColumnEncoder>(options);
}

std::shared_ptr<const ColumnFactory> CreateV1ColumnFactory() {
  return std::make_shared<V1ColumnFactory>();
}

}  // namespace borophene::storage
