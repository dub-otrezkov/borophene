#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borophene/common/error.hpp"
#include "borophene/common/result.hpp"
#include "borophene/data/column_vector.hpp"
#include "borophene/data/data_chunk.hpp"
#include "borophene/data/logical_type.hpp"
#include "borophene/data/schema.hpp"
#include "borophene/execution/pipeline.hpp"
#include "borophene/execution/workers/columnar_reader.hpp"
#include "borophene/execution/workers/columnar_writer.hpp"
#include "borophene/io/file.hpp"
#include "borophene/storage/columnar_format.hpp"

namespace {

using borophene::Byte;
using borophene::ColumnVector;
using borophene::DataChunk;
using borophene::ErrorCode;
using borophene::Field;
using borophene::i32;
using borophene::Index;
using borophene::LogicalType;
using borophene::Result;
using borophene::Schema;
using borophene::ui32;
using borophene::ui64;
using borophene::ui8;
using borophene::ValidityMask;
using borophene::execution::ColumnarReader;
using borophene::execution::ColumnarWriter;
using borophene::io::MemoryStream;
using borophene::io::OutputStream;
using borophene::io::RandomAccessFile;
using borophene::storage::ColumnCodecOptions;
using borophene::storage::ColumnCompression;
using borophene::storage::ColumnDecodeOptions;
using borophene::storage::ColumnEncoder;
using borophene::storage::ColumnEncoding;
using borophene::storage::ColumnFactory;
using borophene::storage::CreateV1ColumnEncoder;
using borophene::storage::CreateV1ColumnFactory;
using borophene::storage::SerializedColumn;

using Buffer = std::vector<Byte>;

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

class ExtremePositionOutput final : public OutputStream {
 public:
  ui64 Position() const noexcept override {
    if (!wrote_header_) {
      return 0;
    }
    if (!reported_header_position_) {
      reported_header_position_ = true;
      return borophene::storage::kColumnarHeaderSize;
    }
    return std::numeric_limits<ui64>::max();
  }

  Result<void> Write(std::span<const Byte>) override {
    wrote_header_ = true;
    return {};
  }

  Result<void> Flush() override {
    return {};
  }

 private:
  bool wrote_header_ = false;
  mutable bool reported_header_position_ = false;
};

class StalledPositionOutput final : public OutputStream {
 public:
  ui64 Position() const noexcept override {
    return 0;
  }

  Result<void> Write(std::span<const Byte>) override {
    return {};
  }

  Result<void> Flush() override {
    return {};
  }
};

class ShortReadInput final : public RandomAccessFile {
 public:
  explicit ShortReadInput(Buffer bytes) : bytes_(std::move(bytes)) {
  }

  Result<ui64> Size() const override {
    return bytes_.size();
  }

  Result<std::size_t> ReadAt(ui64 offset, std::span<Byte> destination) const override {
    if (offset >= bytes_.size()) {
      return std::size_t{0};
    }
    const auto size = std::min<std::size_t>({destination.size(), bytes_.size() - static_cast<std::size_t>(offset), 3});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), size, destination.begin());
    return size;
  }

 private:
  Buffer bytes_;
};

class RecordingColumnEncoder final : public ColumnEncoder {
 public:
  explicit RecordingColumnEncoder(std::shared_ptr<const ColumnEncoder> delegate) : delegate_(std::move(delegate)) {
  }

  Result<SerializedColumn> Serialize(const ColumnVector& column, const Field& field) const override {
    ++call_count_;
    return delegate_->Serialize(column, field);
  }

  std::size_t CallCount() const noexcept {
    return call_count_;
  }

 private:
  std::shared_ptr<const ColumnEncoder> delegate_;
  mutable std::size_t call_count_ = 0;
};

class RecordingColumnFactory final : public ColumnFactory {
 public:
  explicit RecordingColumnFactory(std::shared_ptr<const ColumnFactory> delegate) : delegate_(std::move(delegate)) {
  }

  Result<ColumnVector> Create(std::span<const Byte> bytes, const Field& field, Index row_count,
                              const ColumnDecodeOptions& options) const override {
    ++call_count_;
    return delegate_->Create(bytes, field, row_count, options);
  }

  std::size_t CallCount() const noexcept {
    return call_count_;
  }

 private:
  std::shared_ptr<const ColumnFactory> delegate_;
  mutable std::size_t call_count_ = 0;
};

class FixedColumnEncoder final : public ColumnEncoder {
 public:
  explicit FixedColumnEncoder(SerializedColumn serialized) : serialized_(std::move(serialized)) {
  }

  Result<SerializedColumn> Serialize(const ColumnVector&, const Field&) const override {
    return serialized_;
  }

 private:
  SerializedColumn serialized_;
};

class FixedColumnFactory final : public ColumnFactory {
 public:
  explicit FixedColumnFactory(ColumnVector column) : column_(std::move(column)) {
  }

  Result<ColumnVector> Create(std::span<const Byte>, const Field&, Index, const ColumnDecodeOptions&) const override {
    return column_;
  }

 private:
  ColumnVector column_;
};

std::unique_ptr<MemoryStream> MemoryInput(std::span<const Byte> bytes) {
  return std::make_unique<MemoryStream>(bytes);
}

Buffer CopyBytes(const MemoryStream& stream) {
  return {stream.Data().begin(), stream.Data().end()};
}

ui8 U8(std::span<const Byte> bytes, std::size_t offset) {
  return std::to_integer<ui8>(bytes[offset]);
}

ui32 U32(std::span<const Byte> bytes, std::size_t offset) {
  ui32 value = 0;
  for (unsigned int index = 0; index < 4; ++index) {
    value |= static_cast<ui32>(U8(bytes, offset + index)) << (8U * index);
  }
  return value;
}

ui64 U64(std::span<const Byte> bytes, std::size_t offset) {
  ui64 value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value |= static_cast<ui64>(U8(bytes, offset + index)) << (8U * index);
  }
  return value;
}

void SetU32(Buffer& bytes, std::size_t offset, ui32 value) {
  for (unsigned int index = 0; index < 4; ++index) {
    bytes.at(offset + index) = static_cast<Byte>(static_cast<ui8>(value >> (8U * index)));
  }
}

void SetU64(Buffer& bytes, std::size_t offset, ui64 value) {
  for (unsigned int index = 0; index < 8; ++index) {
    bytes.at(offset + index) = static_cast<Byte>(static_cast<ui8>(value >> (8U * index)));
  }
}

Schema IntSchema(bool nullable = false) {
  return Schema::Create({Field{.name = "id", .type = LogicalType::kInt32, .nullable = nullable}}).value();
}

Schema MixedSchema() {
  return Schema::Create({Field{.name = "id", .type = LogicalType::kInt32, .nullable = true},
                         Field{.name = "name", .type = LogicalType::kString, .nullable = true}})
      .value();
}

DataChunk IntChunk(std::vector<i32> values) {
  auto column = ColumnVector::CreateInt32(std::move(values)).value();
  return DataChunk::Create(IntSchema(), {std::move(column)}).value();
}

SerializedColumn PlainInt32Column() {
  return SerializedColumn{.bytes = {Byte{1}, Byte{0}, Byte{0}, Byte{0}},
                          .decoded_size = sizeof(i32),
                          .null_count = 0,
                          .encoding = ColumnEncoding::kPlain,
                          .compression = ColumnCompression::kNone};
}

Result<void> WriteIntWithEncoder(std::shared_ptr<const ColumnEncoder> encoder) {
  auto writer = ColumnarWriter::Create(std::make_unique<MemoryStream>(), std::move(encoder));
  if (!writer) {
    return borophene::MakeUnexpected(std::move(writer.error()));
  }
  auto begun = writer->Begin(IntSchema());
  if (!begun) {
    return begun;
  }
  return writer->WriteRowGroup(IntChunk({1}));
}

DataChunk MixedChunk(std::vector<i32> ids, ValidityMask id_validity, std::vector<std::string> names,
                     ValidityMask name_validity) {
  auto id_column = ColumnVector::CreateInt32(std::move(ids), std::move(id_validity)).value();
  auto name_column = ColumnVector::CreateString(std::move(names), std::move(name_validity)).value();
  return DataChunk::Create(MixedSchema(), {std::move(id_column), std::move(name_column)}).value();
}

const ColumnVector& Column(const DataChunk& chunk, Index index) {
  return chunk.Column(index).value().get();
}

const std::vector<i32>& Int32Values(const DataChunk& chunk, Index index) {
  return Column(chunk, index).Int32Values().value().get();
}

const std::vector<std::string>& StringValues(const DataChunk& chunk, Index index) {
  return Column(chunk, index).StringValues().value().get();
}

bool IsValid(const DataChunk& chunk, Index column, Index row) {
  return Column(chunk, column).Validity().IsValid(row).value();
}

std::unique_ptr<MemoryStream> WriteEmptyTable(const Schema& schema) {
  auto output = std::make_unique<MemoryStream>();
  auto storage = output->Share();
  auto writer = ColumnarWriter::Create(std::move(output)).value();
  writer.Begin(schema).value();
  writer.Finish().value();
  return storage;
}

std::unique_ptr<MemoryStream> WriteMixedTable() {
  auto int_validity = ValidityMask::Create(4).value();
  int_validity.SetValid(1, false).value();
  auto string_validity = ValidityMask::Create(4).value();
  string_validity.SetValid(2, false).value();

  auto ids = ColumnVector::CreateInt32({1, 999, -2, 0}, std::move(int_validity)).value();
  auto names = ColumnVector::CreateString({"alpha", "", "ignored", "z"}, std::move(string_validity)).value();
  auto chunk = DataChunk::Create(MixedSchema(), {std::move(ids), std::move(names)}).value();

  auto output = std::make_unique<MemoryStream>();
  auto storage = output->Share();
  auto writer = ColumnarWriter::Create(std::move(output)).value();
  writer.Begin(MixedSchema()).value();
  writer.WriteRowGroup(chunk).value();
  writer.Finish().value();
  return storage;
}

void TestDeterministicEnvelopeAndEmptyTable() {
  const auto storage = WriteEmptyTable(IntSchema());
  const auto bytes = storage->Data();
  Check(storage->IsFlushed(), "finish flushes the output");
  Check(bytes.size() == 82, "empty-table representation has deterministic size");
  Check(U8(bytes, 0) == 'B' && U8(bytes, 7) == 'N', "header magic is BOROPHEN");
  Check(U8(bytes, 8) == 1 && U8(bytes, 9) == 0 && U8(bytes, 10) == 0 && U8(bytes, 11) == 0,
        "version uses explicit little endian");
  Check(U32(bytes, 12) == 0, "header flags are zero");

  const std::size_t trailer = bytes.size() - borophene::storage::kColumnarTrailerSize;
  Check(U8(bytes, trailer) == 'B' && U8(bytes, trailer + 4) == '_' && U8(bytes, trailer + 7) == 'D',
        "trailer magic is BORO_END");
  Check(U64(bytes, trailer + 16) == 16 && U64(bytes, trailer + 24) == 26 && U64(bytes, trailer + 32) == bytes.size(),
        "trailer offsets and sizes are little endian");

  auto reader = ColumnarReader::Open(storage->Share());
  Check(reader && reader->GetSchema() == IntSchema(), "empty table preserves its schema");
  Check(reader && reader->RowCount() == 0 && reader->RowGroupCount() == 0, "empty table has no rows or groups");
  auto next = reader->Next();
  Check(next && !next->has_value(), "empty reader immediately reaches EOF");

  auto short_read_reader = ColumnarReader::Open(std::make_unique<ShortReadInput>(CopyBytes(*storage)));
  Check(short_read_reader && short_read_reader->RowCount() == 0, "reader supports legal partial file reads");
}

void TestMixedRoundTrip() {
  const auto storage = WriteMixedTable();
  auto reader = ColumnarReader::Open(storage->Share()).value();
  Check(reader.RowCount() == 4 && reader.RowGroupCount() == 1, "mixed table row counts round-trip");
  auto chunk = reader.ReadRowGroup(0).value();
  Check(Int32Values(chunk, 0) == std::vector<i32>({1, 0, -2, 0}), "int32 values use canonical zero for null rows");
  Check(!IsValid(chunk, 0, 1) && Column(chunk, 0).Validity().NullCount() == 1, "int32 validity round-trips");
  Check(StringValues(chunk, 1) == std::vector<std::string>({"alpha", "", "", "z"}),
        "string bytes round-trip with canonical empty null storage");
  Check(IsValid(chunk, 1, 1) && !IsValid(chunk, 1, 2), "empty string remains distinct from null");
  Check(!reader.ReadRowGroup(1) && reader.ReadRowGroup(1).error().Code() == ErrorCode::kOutOfRange,
        "random row-group reads are bounds checked");
}

void TestMultipleGroupsAndIteration() {
  auto output = std::make_unique<MemoryStream>();
  auto storage = output->Share();
  auto writer = ColumnarWriter::Create(std::move(output)).value();
  writer.Begin(IntSchema()).value();
  writer.WriteRowGroup(IntChunk({1, 2})).value();
  writer.WriteRowGroup(IntChunk({3})).value();
  writer.Finish().value();

  auto reader = ColumnarReader::Open(storage->Share()).value();
  Check(reader.RowCount() == 3 && reader.RowGroupCount() == 2, "multiple row groups retain counts");
  auto first = reader.Next();
  auto second = reader.Next();
  auto end = reader.Next();
  if (first && first->has_value()) {
    Check(Int32Values(first->value(), 0) == std::vector<i32>({1, 2}), "iteration reads the first row group");
  } else {
    Check(false, "iteration reads the first row group");
  }
  if (second && second->has_value()) {
    Check(Int32Values(second->value(), 0) == std::vector<i32>({3}), "iteration reads the second row group");
  } else {
    Check(false, "iteration reads the second row group");
  }
  Check(end && !end->has_value(), "iteration reports explicit EOF");
  reader.Reset();
  auto restarted = reader.Next();
  Check(restarted && restarted->has_value() && Int32Values(**restarted, 0).front() == 1,
        "reset restarts row-group iteration");
}

void TestStoragePipelineRoundTrip() {
  auto first_id_validity = ValidityMask::Create(2).value();
  first_id_validity.SetValid(1, false).value();
  auto first_name_validity = ValidityMask::Create(2).value();
  first_name_validity.SetValid(0, false).value();
  auto second_id_validity = ValidityMask::Create(1).value();
  auto second_name_validity = ValidityMask::Create(1).value();
  second_name_validity.SetValid(0, false).value();

  auto source_output = std::make_unique<MemoryStream>();
  auto source_storage = source_output->Share();
  auto source_writer = ColumnarWriter::Create(std::move(source_output)).value();
  source_writer.Begin(MixedSchema()).value();
  source_writer
      .WriteRowGroup(
          MixedChunk({7, 99}, std::move(first_id_validity), {"ignored", "ok"}, std::move(first_name_validity)))
      .value();
  source_writer
      .WriteRowGroup(MixedChunk({8}, std::move(second_id_validity), {"ignored"}, std::move(second_name_validity)))
      .value();
  source_writer.Finish().value();

  auto source = ColumnarReader::Open(source_storage->Share()).value();
  auto destination_output = std::make_unique<MemoryStream>();
  auto destination_storage = destination_output->Share();
  auto sink = ColumnarWriter::Create(std::move(destination_output)).value();
  auto pipeline = borophene::execution::RunPipeline(source, sink);
  Check(pipeline.has_value(), "columnar reader and writer satisfy the execution pipeline contracts");

  auto result = ColumnarReader::Open(destination_storage->Share()).value();
  Check(result.RowCount() == 3 && result.RowGroupCount() == 2, "pipeline preserves row-group boundaries");
  auto first = result.ReadRowGroup(0).value();
  auto second = result.ReadRowGroup(1).value();
  Check(!IsValid(first, 0, 1) && !IsValid(first, 1, 0), "pipeline preserves nullable INT32 and STRING values");
  Check(Int32Values(second, 0).front() == 8 && !IsValid(second, 1, 0), "pipeline preserves the final short row group");
}

void TestWriterValidation() {
  auto writer = ColumnarWriter::Create(std::make_unique<MemoryStream>()).value();
  Check(!writer.WriteRowGroup(IntChunk({1})), "write before begin is rejected");
  Check(!writer.Finish(), "finish before begin is rejected");
  Check(writer.Begin(IntSchema()).has_value(), "begin accepts a schema once");
  Check(!writer.Begin(IntSchema()), "double begin is rejected");

  auto empty_column = ColumnVector::CreateInt32({}).value();
  auto empty_chunk = DataChunk::Create(IntSchema(), {std::move(empty_column)}).value();
  Check(!writer.WriteRowGroup(empty_chunk), "empty row groups are rejected");

  auto string_schema = Schema::Create({Field{.name = "id", .type = LogicalType::kString, .nullable = false}}).value();
  auto string_column = ColumnVector::CreateString({"1"}).value();
  auto wrong_type = DataChunk::Create(string_schema, {std::move(string_column)}).value();
  auto mismatch = writer.WriteRowGroup(wrong_type);
  Check(!mismatch && mismatch.error().Code() == ErrorCode::kSchemaMismatch, "row-group type mismatch is rejected");

  Check(writer.WriteRowGroup(IntChunk({1})).has_value(), "valid row group is accepted");
  Check(writer.Finish().has_value(), "finish succeeds once");
  Check(!writer.Finish(), "double finish is rejected");
  Check(!writer.WriteRowGroup(IntChunk({2})), "write after finish is rejected");
  Check(!ColumnarWriter::Create(std::unique_ptr<OutputStream>()), "null output is rejected");
  Check(!ColumnarReader::Open(std::unique_ptr<RandomAccessFile>()), "null input is rejected");

  auto stalled_writer = ColumnarWriter::Create(std::make_unique<StalledPositionOutput>()).value();
  auto stalled_begin = stalled_writer.Begin(IntSchema());
  Check(!stalled_begin && stalled_begin.error().Code() == ErrorCode::kIo,
        "writer rejects an output stream with an inconsistent logical position");

  auto overflow_writer = ColumnarWriter::Create(std::make_unique<ExtremePositionOutput>()).value();
  overflow_writer.Begin(IntSchema()).value();
  Check(!overflow_writer.Finish(), "file-size overflow is rejected before trailer serialization");
}

void TestCorruptEnvelope() {
  const auto storage = WriteEmptyTable(IntSchema());
  const auto valid = CopyBytes(*storage);

  auto corrupt_header = valid;
  corrupt_header[0] = Byte{0};
  Check(!ColumnarReader::Open(MemoryInput(corrupt_header)), "corrupt header magic is rejected");

  auto corrupt_trailer = valid;
  corrupt_trailer[corrupt_trailer.size() - borophene::storage::kColumnarTrailerSize] = Byte{0};
  Check(!ColumnarReader::Open(MemoryInput(corrupt_trailer)), "corrupt trailer magic is rejected");

  auto unsupported = valid;
  unsupported[8] = Byte{2};
  auto unsupported_result = ColumnarReader::Open(MemoryInput(unsupported));
  Check(!unsupported_result && unsupported_result.error().Code() == ErrorCode::kUnsupportedVersion,
        "unsupported versions have a distinct error");

  auto unsupported_trailer = valid;
  unsupported_trailer[unsupported_trailer.size() - borophene::storage::kColumnarTrailerSize + 8] = Byte{2};
  auto unsupported_trailer_result = ColumnarReader::Open(MemoryInput(unsupported_trailer));
  Check(!unsupported_trailer_result && unsupported_trailer_result.error().Code() == ErrorCode::kUnsupportedVersion,
        "unsupported trailer versions are rejected");

  auto wrong_file_size = valid;
  SetU64(wrong_file_size, wrong_file_size.size() - 8, wrong_file_size.size() + 1);
  Check(!ColumnarReader::Open(MemoryInput(wrong_file_size)), "trailer file-size disagreement is rejected");

  auto truncated = valid;
  truncated.pop_back();
  Check(!ColumnarReader::Open(MemoryInput(truncated)), "truncated files are rejected");
}

void TestCorruptMetadataAndPayload() {
  const auto storage = WriteMixedTable();
  const auto mixed = CopyBytes(*storage);
  const std::size_t trailer = mixed.size() - borophene::storage::kColumnarTrailerSize;
  const auto metadata = static_cast<std::size_t>(U64(mixed, trailer + 16));

  auto unknown_type = mixed;
  const std::size_t first_type = metadata + 16 + 4 + U32(mixed, metadata + 16);
  unknown_type[first_type] = Byte{99};
  Check(!ColumnarReader::Open(MemoryInput(unknown_type)), "unknown serialized logical types are rejected");

  auto chunk_out_of_range = mixed;
  const std::size_t field_one_size = 4 + U32(mixed, metadata + 16) + 4;
  const std::size_t field_two_at = metadata + 16 + field_one_size;
  const std::size_t field_two_size = 4 + U32(mixed, field_two_at) + 4;
  const std::size_t first_chunk_offset = field_two_at + field_two_size + 16;
  SetU64(chunk_out_of_range, first_chunk_offset, metadata);
  Check(!ColumnarReader::Open(MemoryInput(chunk_out_of_range)), "chunks extending into metadata are rejected");

  auto unknown_encoding = mixed;
  unknown_encoding[first_chunk_offset + 28] = Byte{99};
  auto unused_factory = std::make_shared<RecordingColumnFactory>(CreateV1ColumnFactory());
  Check(!ColumnarReader::Open(MemoryInput(unknown_encoding), unused_factory) && unused_factory->CallCount() == 0,
        "unknown column encodings are rejected before invoking a factory");

  auto unknown_compression = mixed;
  unknown_compression[first_chunk_offset + 29] = Byte{99};
  Check(!ColumnarReader::Open(MemoryInput(unknown_compression)),
        "unknown column compression identifiers are rejected in metadata");

  const std::size_t second_chunk_metadata = first_chunk_offset + 32;
  auto uncompressed_size_mismatch = mixed;
  SetU64(uncompressed_size_mismatch, second_chunk_metadata + 16, U64(mixed, second_chunk_metadata + 16) + 1U);
  Check(!ColumnarReader::Open(MemoryInput(uncompressed_size_mismatch)),
        "uncompressed stored and decoded sizes must agree");

  auto invalid_string_offsets = mixed;
  const auto string_payload = static_cast<std::size_t>(U64(mixed, second_chunk_metadata));
  const std::size_t string_bitmap_size = 1;
  SetU32(invalid_string_offsets, string_payload + string_bitmap_size + sizeof(ui32), std::numeric_limits<ui32>::max());
  auto reader = ColumnarReader::Open(MemoryInput(invalid_string_offsets));
  Check(reader && !reader->ReadRowGroup(0), "malformed string offsets are rejected while decoding");

  auto bad_validity_padding = mixed;
  bad_validity_padding[16] |= Byte{0x80};
  auto padding_reader = ColumnarReader::Open(MemoryInput(bad_validity_padding));
  Check(padding_reader && !padding_reader->ReadRowGroup(0), "non-zero validity padding bits are rejected");
}

void TestCodecExtensionBoundary() {
  auto encoder = std::make_shared<RecordingColumnEncoder>(
      CreateV1ColumnEncoder(ColumnCodecOptions{.compression = ColumnCompression::kZstd}));
  auto writer = ColumnarWriter::Create(std::make_unique<MemoryStream>(), encoder).value();
  writer.Begin(IntSchema()).value();
  auto write = writer.WriteRowGroup(IntChunk({1}));
  Check(!write && write.error().Code() == ErrorCode::kUnsupportedVersion,
        "writer reports unsupported compression through the codec boundary");
  Check(encoder->CallCount() == 1, "writer delegates column serialization to its injected encoder");

  const auto storage = WriteMixedTable();
  auto unsupported = CopyBytes(*storage);
  const std::size_t trailer = unsupported.size() - borophene::storage::kColumnarTrailerSize;
  const auto metadata = static_cast<std::size_t>(U64(unsupported, trailer + 16));
  const std::size_t field_one_size = 4 + U32(unsupported, metadata + 16) + 4;
  const std::size_t field_two_at = metadata + 16 + field_one_size;
  const std::size_t field_two_size = 4 + U32(unsupported, field_two_at) + 4;
  const std::size_t first_chunk_metadata = field_two_at + field_two_size + 16;
  unsupported[first_chunk_metadata + 29] = static_cast<Byte>(ColumnCompression::kZstd);

  auto column_factory = std::make_shared<RecordingColumnFactory>(CreateV1ColumnFactory());
  auto reader = ColumnarReader::Open(MemoryInput(unsupported), column_factory);
  Check(reader.has_value(), "reader accepts extension metadata before payload decoding");
  auto read = reader->ReadRowGroup(0);
  Check(!read && read.error().Code() == ErrorCode::kUnsupportedVersion,
        "reader reports unsupported compression through the codec boundary");
  Check(column_factory->CallCount() == 1, "reader delegates column construction to its injected factory");

  const Field field{.name = "id", .type = LogicalType::kInt32, .nullable = false};
  auto oversized_decode = CreateV1ColumnFactory()->Create(
      {}, field, 1,
      ColumnDecodeOptions{.decoded_size = borophene::storage::kColumnarMaxChunkSize + 1U,
                          .null_count = 0,
                          .encoding = ColumnEncoding::kPlain,
                          .compression = ColumnCompression::kNone});
  Check(!oversized_decode && oversized_decode.error().Code() == ErrorCode::kInvalidFormat,
        "public column factory rejects decoded sizes above the format limit");
  auto oversized_row_count =
      CreateV1ColumnFactory()->Create({}, field, static_cast<Index>(std::numeric_limits<ui32>::max()) + 1U,
                                      ColumnDecodeOptions{.decoded_size = 0,
                                                          .null_count = 0,
                                                          .encoding = ColumnEncoding::kPlain,
                                                          .compression = ColumnCompression::kNone});
  Check(!oversized_row_count && oversized_row_count.error().Code() == ErrorCode::kInvalidFormat,
        "public column factory rejects row counts above the format limit");
}

void TestInjectedCodecContracts() {
  auto invalid_null_count = PlainInt32Column();
  invalid_null_count.null_count = 1;
  auto null_count_result = WriteIntWithEncoder(std::make_shared<FixedColumnEncoder>(invalid_null_count));
  Check(!null_count_result && null_count_result.error().Code() == ErrorCode::kInvalidArgument,
        "writer rejects encoder null counts that disagree with the source column");

  auto oversized_decoded = PlainInt32Column();
  oversized_decoded.decoded_size = borophene::storage::kColumnarMaxChunkSize + 1U;
  auto oversized_result = WriteIntWithEncoder(std::make_shared<FixedColumnEncoder>(oversized_decoded));
  Check(!oversized_result && oversized_result.error().Code() == ErrorCode::kInvalidArgument,
        "writer rejects encoder metadata above the chunk limit");

  auto unknown_encoding = PlainInt32Column();
  unknown_encoding.encoding = static_cast<ColumnEncoding>(99);  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  auto encoding_result = WriteIntWithEncoder(std::make_shared<FixedColumnEncoder>(unknown_encoding));
  Check(!encoding_result && encoding_result.error().Code() == ErrorCode::kUnsupportedVersion,
        "writer rejects unknown encoder identifiers");

  auto unknown_compression = PlainInt32Column();
  unknown_compression.compression =
      static_cast<ColumnCompression>(99);  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  auto compression_result = WriteIntWithEncoder(std::make_shared<FixedColumnEncoder>(unknown_compression));
  Check(!compression_result && compression_result.error().Code() == ErrorCode::kUnsupportedVersion,
        "writer rejects unknown compression identifiers");

  auto inconsistent_size = PlainInt32Column();
  inconsistent_size.decoded_size += 1U;
  auto size_result = WriteIntWithEncoder(std::make_shared<FixedColumnEncoder>(inconsistent_size));
  Check(!size_result && size_result.error().Code() == ErrorCode::kInvalidArgument,
        "writer rejects inconsistent uncompressed sizes");

  auto compressed = PlainInt32Column();
  compressed.bytes = {Byte{42}};
  compressed.compression = ColumnCompression::kZstd;
  auto compressed_output = std::make_unique<MemoryStream>();
  auto compressed_storage = compressed_output->Share();
  auto compressed_writer =
      ColumnarWriter::Create(std::move(compressed_output), std::make_shared<FixedColumnEncoder>(compressed)).value();
  compressed_writer.Begin(IntSchema()).value();
  Check(compressed_writer.WriteRowGroup(IntChunk({1})).has_value() && compressed_writer.Finish().has_value(),
        "writer orchestration accepts a known compression implemented by an injected encoder");

  auto default_reader = ColumnarReader::Open(compressed_storage->Share()).value();
  auto unsupported_read = default_reader.ReadRowGroup(0);
  Check(!unsupported_read && unsupported_read.error().Code() == ErrorCode::kUnsupportedVersion,
        "built-in factory reports its unsupported compression explicitly");
  auto custom_factory = std::make_shared<FixedColumnFactory>(ColumnVector::CreateInt32({7}).value());
  auto custom_reader = ColumnarReader::Open(compressed_storage->Share(), custom_factory).value();
  auto custom_chunk = custom_reader.ReadRowGroup(0);
  Check(custom_chunk && Int32Values(*custom_chunk, 0) == std::vector<i32>({7}),
        "injected encoder and factory can extend the compression stage without changing orchestration");

  const auto mixed_storage = WriteMixedTable();
  auto short_factory = std::make_shared<FixedColumnFactory>(ColumnVector::CreateInt32({1}).value());
  auto short_reader = ColumnarReader::Open(mixed_storage->Share(), short_factory).value();
  auto short_column = short_reader.ReadRowGroup(0);
  Check(!short_column && short_column.error().Code() == ErrorCode::kInvalidFormat,
        "reader rejects factory output with a row count that disagrees with metadata");

  auto valid_values = ColumnVector::CreateInt32({1, 2, 3, 4}).value();
  auto wrong_null_factory = std::make_shared<FixedColumnFactory>(std::move(valid_values));
  auto wrong_null_reader = ColumnarReader::Open(mixed_storage->Share(), wrong_null_factory).value();
  auto wrong_null_column = wrong_null_reader.ReadRowGroup(0);
  Check(!wrong_null_column && wrong_null_column.error().Code() == ErrorCode::kInvalidFormat,
        "reader rejects factory validity that disagrees with metadata");

  auto wrong_type = ColumnVector::CreateString({"a", "b", "c", "d"}).value();
  auto wrong_type_factory = std::make_shared<FixedColumnFactory>(std::move(wrong_type));
  auto wrong_type_reader = ColumnarReader::Open(mixed_storage->Share(), wrong_type_factory).value();
  auto wrong_type_column = wrong_type_reader.ReadRowGroup(0);
  Check(!wrong_type_column && wrong_type_column.error().Code() == ErrorCode::kInvalidFormat,
        "reader rejects factory output with a type that disagrees with the schema");
}

}  // namespace

int main() {
  try {
    TestDeterministicEnvelopeAndEmptyTable();
    TestMixedRoundTrip();
    TestMultipleGroupsAndIteration();
    TestStoragePipelineRoundTrip();
    TestWriterValidation();
    TestCorruptEnvelope();
    TestCorruptMetadataAndPayload();
    TestCodecExtensionBoundary();
    TestInjectedCodecContracts();
  } catch (const std::exception& exception) {
    std::cerr << "unexpected exception: " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "unexpected non-standard exception\n";
    return 1;
  }
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
  }
  return failures == 0 ? 0 : 1;
}
