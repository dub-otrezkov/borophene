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
#include "borophene/io/file.hpp"
#include "borophene/storage/columnar_format.hpp"
#include "borophene/storage/columnar_reader.hpp"
#include "borophene/storage/columnar_writer.hpp"

namespace {

using borophene::ColumnVector;
using borophene::DataChunk;
using borophene::ErrorCode;
using borophene::Field;
using borophene::Index;
using borophene::LogicalType;
using borophene::Result;
using borophene::Schema;
using borophene::ValidityMask;
using borophene::io::OutputFile;
using borophene::io::RandomAccessFile;
using borophene::storage::ColumnarReader;
using borophene::storage::ColumnarWriter;

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

struct MemoryStorage {
  std::vector<std::byte> bytes;
  bool flushed = false;
};

class MemoryOutput final : public OutputFile {
 public:
  explicit MemoryOutput(std::shared_ptr<MemoryStorage> storage) : storage_(std::move(storage)) {
  }

  std::uint64_t Position() const noexcept override {
    return storage_->bytes.size();
  }

  Result<void> Write(std::span<const std::byte> data) override {
    storage_->bytes.insert(storage_->bytes.end(), data.begin(), data.end());
    return {};
  }

  Result<void> Flush() override {
    storage_->flushed = true;
    return {};
  }

 private:
  std::shared_ptr<MemoryStorage> storage_;
};

class MemoryInput : public RandomAccessFile {
 public:
  explicit MemoryInput(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {
  }

  Result<std::uint64_t> Size() const override {
    return bytes_.size();
  }

  Result<std::size_t> ReadAt(std::uint64_t offset, std::span<std::byte> destination) const override {
    if (offset >= bytes_.size()) {
      return std::size_t{0};
    }
    const std::size_t size =
        std::min<std::size_t>(destination.size(), bytes_.size() - static_cast<std::size_t>(offset));
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), size, destination.begin());
    return size;
  }

 private:
  std::vector<std::byte> bytes_;
};

class ExtremePositionOutput final : public OutputFile {
 public:
  std::uint64_t Position() const noexcept override {
    return wrote_header_ ? std::numeric_limits<std::uint64_t>::max() : 0;
  }

  Result<void> Write(std::span<const std::byte>) override {
    wrote_header_ = true;
    return {};
  }

  Result<void> Flush() override {
    return {};
  }

 private:
  bool wrote_header_ = false;
};

class ShortReadInput final : public RandomAccessFile {
 public:
  explicit ShortReadInput(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {
  }

  Result<std::uint64_t> Size() const override {
    return bytes_.size();
  }

  Result<std::size_t> ReadAt(std::uint64_t offset, std::span<std::byte> destination) const override {
    if (offset >= bytes_.size()) {
      return std::size_t{0};
    }
    const auto size = std::min<std::size_t>({destination.size(), bytes_.size() - static_cast<std::size_t>(offset), 3});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), size, destination.begin());
    return size;
  }

 private:
  std::vector<std::byte> bytes_;
};

std::uint8_t U8(const std::vector<std::byte>& bytes, std::size_t offset) {
  return std::to_integer<std::uint8_t>(bytes.at(offset));
}

std::uint32_t U32(const std::vector<std::byte>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(U8(bytes, offset + index)) << (8U * index);
  }
  return value;
}

std::uint64_t U64(const std::vector<std::byte>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(U8(bytes, offset + index)) << (8U * index);
  }
  return value;
}

void SetU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned int index = 0; index < 4; ++index) {
    bytes.at(offset + index) = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * index)));
  }
}

void SetU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index) {
    bytes.at(offset + index) = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * index)));
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

DataChunk IntChunk(std::vector<std::int32_t> values) {
  auto column = ColumnVector::CreateInt32(std::move(values)).value();
  return DataChunk::Create(IntSchema(), {std::move(column)}).value();
}

DataChunk MixedChunk(std::vector<std::int32_t> ids, ValidityMask id_validity, std::vector<std::string> names,
                     ValidityMask name_validity) {
  auto id_column = ColumnVector::CreateInt32(std::move(ids), std::move(id_validity)).value();
  auto name_column = ColumnVector::CreateString(std::move(names), std::move(name_validity)).value();
  return DataChunk::Create(MixedSchema(), {std::move(id_column), std::move(name_column)}).value();
}

std::shared_ptr<MemoryStorage> WriteEmptyTable(const Schema& schema) {
  auto storage = std::make_shared<MemoryStorage>();
  auto writer = ColumnarWriter::Create(std::make_unique<MemoryOutput>(storage)).value();
  writer.Begin(schema).value();
  writer.Finish().value();
  return storage;
}

std::shared_ptr<MemoryStorage> WriteMixedTable() {
  ValidityMask int_validity(4);
  int_validity.SetValid(1, false);
  ValidityMask string_validity(4);
  string_validity.SetValid(2, false);

  auto ids = ColumnVector::CreateInt32({1, 999, -2, 0}, int_validity).value();
  auto names = ColumnVector::CreateString({"alpha", "", "ignored", "z"}, string_validity).value();
  auto chunk = DataChunk::Create(MixedSchema(), {std::move(ids), std::move(names)}).value();

  auto storage = std::make_shared<MemoryStorage>();
  auto writer = ColumnarWriter::Create(std::make_unique<MemoryOutput>(storage)).value();
  writer.Begin(MixedSchema()).value();
  writer.WriteRowGroup(chunk).value();
  writer.Finish().value();
  return storage;
}

void TestDeterministicEnvelopeAndEmptyTable() {
  const auto storage = WriteEmptyTable(IntSchema());
  const auto& bytes = storage->bytes;
  Check(storage->flushed, "finish flushes the output");
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

  auto reader = ColumnarReader::Open(std::make_unique<MemoryInput>(storage->bytes));
  Check(reader && reader->GetSchema() == IntSchema(), "empty table preserves its schema");
  Check(reader && reader->RowCount() == 0 && reader->RowGroupCount() == 0, "empty table has no rows or groups");
  auto next = reader->Next();
  Check(next && !next->has_value(), "empty reader immediately reaches EOF");

  auto short_read_reader = ColumnarReader::Open(std::make_unique<ShortReadInput>(bytes));
  Check(short_read_reader && short_read_reader->RowCount() == 0, "reader supports legal partial file reads");
}

void TestMixedRoundTrip() {
  const auto storage = WriteMixedTable();
  auto reader = ColumnarReader::Open(std::make_unique<MemoryInput>(storage->bytes)).value();
  Check(reader.RowCount() == 4 && reader.RowGroupCount() == 1, "mixed table row counts round-trip");
  auto chunk = reader.ReadRowGroup(0).value();
  Check(chunk.Column(0).Int32Values() == std::vector<std::int32_t>({1, 0, -2, 0}),
        "int32 values use canonical zero for null rows");
  Check(!chunk.Column(0).Validity().IsValid(1) && chunk.Column(0).Validity().NullCount() == 1,
        "int32 validity round-trips");
  Check(chunk.Column(1).StringValues() == std::vector<std::string>({"alpha", "", "", "z"}),
        "string bytes round-trip with canonical empty null storage");
  Check(chunk.Column(1).Validity().IsValid(1) && !chunk.Column(1).Validity().IsValid(2),
        "empty string remains distinct from null");
  Check(!reader.ReadRowGroup(1) && reader.ReadRowGroup(1).error().Code() == ErrorCode::kOutOfRange,
        "random row-group reads are bounds checked");
}

void TestMultipleGroupsAndIteration() {
  auto storage = std::make_shared<MemoryStorage>();
  auto writer = ColumnarWriter::Create(std::make_unique<MemoryOutput>(storage)).value();
  writer.Begin(IntSchema()).value();
  writer.WriteRowGroup(IntChunk({1, 2})).value();
  writer.WriteRowGroup(IntChunk({3})).value();
  writer.Finish().value();

  auto reader = ColumnarReader::Open(std::make_unique<MemoryInput>(storage->bytes)).value();
  Check(reader.RowCount() == 3 && reader.RowGroupCount() == 2, "multiple row groups retain counts");
  auto first = reader.Next();
  auto second = reader.Next();
  auto end = reader.Next();
  if (first && first->has_value()) {
    Check(first->value().Column(0).Int32Values() == std::vector<std::int32_t>({1, 2}),
          "iteration reads the first row group");
  } else {
    Check(false, "iteration reads the first row group");
  }
  if (second && second->has_value()) {
    Check(second->value().Column(0).Int32Values() == std::vector<std::int32_t>({3}),
          "iteration reads the second row group");
  } else {
    Check(false, "iteration reads the second row group");
  }
  Check(end && !end->has_value(), "iteration reports explicit EOF");
  reader.Reset();
  auto restarted = reader.Next();
  Check(restarted && restarted->has_value() && (*restarted)->Column(0).Int32Values().front() == 1,
        "reset restarts row-group iteration");
}

void TestStoragePipelineRoundTrip() {
  ValidityMask first_id_validity(2);
  first_id_validity.SetValid(1, false);
  ValidityMask first_name_validity(2);
  first_name_validity.SetValid(0, false);
  ValidityMask second_id_validity(1);
  ValidityMask second_name_validity(1);
  second_name_validity.SetValid(0, false);

  auto source_storage = std::make_shared<MemoryStorage>();
  auto source_writer = ColumnarWriter::Create(std::make_unique<MemoryOutput>(source_storage)).value();
  source_writer.Begin(MixedSchema()).value();
  source_writer
      .WriteRowGroup(
          MixedChunk({7, 99}, std::move(first_id_validity), {"ignored", "ok"}, std::move(first_name_validity)))
      .value();
  source_writer
      .WriteRowGroup(MixedChunk({8}, std::move(second_id_validity), {"ignored"}, std::move(second_name_validity)))
      .value();
  source_writer.Finish().value();

  auto source = ColumnarReader::Open(std::make_unique<MemoryInput>(source_storage->bytes)).value();
  auto destination_storage = std::make_shared<MemoryStorage>();
  auto sink = ColumnarWriter::Create(std::make_unique<MemoryOutput>(destination_storage)).value();
  auto pipeline = borophene::execution::RunPipeline(source, sink);
  Check(pipeline.has_value(), "columnar reader and writer satisfy the execution pipeline contracts");

  auto result = ColumnarReader::Open(std::make_unique<MemoryInput>(destination_storage->bytes)).value();
  Check(result.RowCount() == 3 && result.RowGroupCount() == 2, "pipeline preserves row-group boundaries");
  auto first = result.ReadRowGroup(0).value();
  auto second = result.ReadRowGroup(1).value();
  Check(!first.Column(0).Validity().IsValid(1) && !first.Column(1).Validity().IsValid(0),
        "pipeline preserves nullable INT32 and STRING values");
  Check(second.Column(0).Int32Values().front() == 8 && !second.Column(1).Validity().IsValid(0),
        "pipeline preserves the final short row group");
}

void TestWriterValidation() {
  auto storage = std::make_shared<MemoryStorage>();
  auto writer = ColumnarWriter::Create(std::make_unique<MemoryOutput>(storage)).value();
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
  Check(!ColumnarWriter::Create(std::unique_ptr<OutputFile>()), "null output is rejected");
  Check(!ColumnarReader::Open(std::unique_ptr<RandomAccessFile>()), "null input is rejected");

  auto overflow_writer = ColumnarWriter::Create(std::make_unique<ExtremePositionOutput>()).value();
  overflow_writer.Begin(IntSchema()).value();
  Check(!overflow_writer.Finish(), "file-size overflow is rejected before trailer serialization");
}

void TestCorruptEnvelope() {
  const auto valid = WriteEmptyTable(IntSchema())->bytes;

  auto corrupt_header = valid;
  corrupt_header[0] = std::byte{0};
  Check(!ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(corrupt_header))),
        "corrupt header magic is rejected");

  auto corrupt_trailer = valid;
  corrupt_trailer[corrupt_trailer.size() - borophene::storage::kColumnarTrailerSize] = std::byte{0};
  Check(!ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(corrupt_trailer))),
        "corrupt trailer magic is rejected");

  auto unsupported = valid;
  unsupported[8] = std::byte{2};
  auto unsupported_result = ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(unsupported)));
  Check(!unsupported_result && unsupported_result.error().Code() == ErrorCode::kUnsupportedVersion,
        "unsupported versions have a distinct error");

  auto unsupported_trailer = valid;
  unsupported_trailer[unsupported_trailer.size() - borophene::storage::kColumnarTrailerSize + 8] = std::byte{2};
  auto unsupported_trailer_result = ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(unsupported_trailer)));
  Check(!unsupported_trailer_result && unsupported_trailer_result.error().Code() == ErrorCode::kUnsupportedVersion,
        "unsupported trailer versions are rejected");

  auto wrong_file_size = valid;
  SetU64(wrong_file_size, wrong_file_size.size() - 8, wrong_file_size.size() + 1);
  Check(!ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(wrong_file_size))),
        "trailer file-size disagreement is rejected");

  auto truncated = valid;
  truncated.pop_back();
  Check(!ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(truncated))), "truncated files are rejected");
}

void TestCorruptMetadataAndPayload() {
  const auto mixed = WriteMixedTable()->bytes;
  const std::size_t trailer = mixed.size() - borophene::storage::kColumnarTrailerSize;
  const auto metadata = static_cast<std::size_t>(U64(mixed, trailer + 16));

  auto unknown_type = mixed;
  const std::size_t first_type = metadata + 16 + 4 + U32(mixed, metadata + 16);
  unknown_type[first_type] = std::byte{99};
  Check(!ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(unknown_type))),
        "unknown serialized logical types are rejected");

  auto chunk_out_of_range = mixed;
  const std::size_t field_one_size = 4 + U32(mixed, metadata + 16) + 4;
  const std::size_t field_two_at = metadata + 16 + field_one_size;
  const std::size_t field_two_size = 4 + U32(mixed, field_two_at) + 4;
  const std::size_t first_chunk_offset = field_two_at + field_two_size + 16;
  SetU64(chunk_out_of_range, first_chunk_offset, metadata);
  Check(!ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(chunk_out_of_range))),
        "chunks extending into metadata are rejected");

  auto invalid_string_offsets = mixed;
  const std::size_t second_chunk_metadata = first_chunk_offset + 32;
  const auto string_payload = static_cast<std::size_t>(U64(mixed, second_chunk_metadata));
  const std::size_t string_bitmap_size = 1;
  SetU32(invalid_string_offsets, string_payload + string_bitmap_size + sizeof(std::uint32_t),
         std::numeric_limits<std::uint32_t>::max());
  auto reader = ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(invalid_string_offsets)));
  Check(reader && !reader->ReadRowGroup(0), "malformed string offsets are rejected while decoding");

  auto bad_validity_padding = mixed;
  bad_validity_padding[16] |= std::byte{0x80};
  auto padding_reader = ColumnarReader::Open(std::make_unique<MemoryInput>(std::move(bad_validity_padding)));
  Check(padding_reader && !padding_reader->ReadRowGroup(0), "non-zero validity padding bits are rejected");
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
