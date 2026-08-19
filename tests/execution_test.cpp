#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "borophene/common/error.hpp"
#include "borophene/common/result.hpp"
#include "borophene/data/column_vector.hpp"
#include "borophene/data/data_chunk.hpp"
#include "borophene/data/logical_type.hpp"
#include "borophene/data/schema.hpp"
#include "borophene/execution/chunk_sink.hpp"
#include "borophene/execution/chunk_source.hpp"
#include "borophene/execution/pipeline.hpp"

namespace {

using borophene::ColumnVector;
using borophene::DataChunk;
using borophene::ErrorCode;
using borophene::Failure;
using borophene::Field;
using borophene::i32;
using borophene::LogicalType;
using borophene::Result;
using borophene::Schema;
using borophene::ui64;

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

Schema MakeSchema() {
  return Schema::Create({Field{.name = "id", .type = LogicalType::kInt32, .nullable = false}}).value();
}

DataChunk MakeChunk(std::vector<i32> values) {
  auto column = ColumnVector::CreateInt32(std::move(values)).value();
  return DataChunk::Create(MakeSchema(), {std::move(column)}).value();
}

class FakeSource final : public borophene::execution::ChunkSource {
 public:
  explicit FakeSource(std::vector<DataChunk> chunks, bool fail = false)
      : schema_(MakeSchema()), chunks_(std::move(chunks)), fail_(fail) {
  }

  const Schema& GetSchema() const noexcept override {
    return schema_;
  }

  Result<std::optional<DataChunk>> Next() override {
    ++calls_;
    if (fail_) {
      return Failure<std::optional<DataChunk>>(ErrorCode::kIo, "source failed");
    }
    if (next_ == chunks_.size()) {
      return std::optional<DataChunk>{};
    }
    return std::optional<DataChunk>(std::move(chunks_[next_++]));
  }

  std::size_t Calls() const noexcept {
    return calls_;
  }

 private:
  Schema schema_;
  std::vector<DataChunk> chunks_;
  std::size_t next_ = 0;
  std::size_t calls_ = 0;
  bool fail_;
};

class FakeSink final : public borophene::execution::ChunkSink {
 public:
  enum class FailurePoint {
    kNone,
    kBegin,
    kWrite,
    kFinish
  };

  explicit FakeSink(FailurePoint failure = FailurePoint::kNone) : failure_(failure) {
  }

  Result<void> Begin(const Schema& schema) override {
    began_ = true;
    schema_matches_ = schema == MakeSchema();
    if (failure_ == FailurePoint::kBegin) {
      return Failure<void>(ErrorCode::kIo, "begin failed");
    }
    return {};
  }

  Result<void> Write(const DataChunk& chunk) override {
    if (failure_ == FailurePoint::kWrite) {
      return Failure<void>(ErrorCode::kIo, "write failed");
    }
    rows_ += chunk.RowCount();
    ++writes_;
    return {};
  }

  Result<void> Finish() override {
    finished_ = true;
    if (failure_ == FailurePoint::kFinish) {
      return Failure<void>(ErrorCode::kIo, "finish failed");
    }
    return {};
  }

  bool Began() const noexcept {
    return began_;
  }
  bool Finished() const noexcept {
    return finished_;
  }
  bool SchemaMatches() const noexcept {
    return schema_matches_;
  }
  std::size_t Writes() const noexcept {
    return writes_;
  }
  ui64 Rows() const noexcept {
    return rows_;
  }

 private:
  FailurePoint failure_;
  std::size_t writes_ = 0;
  ui64 rows_ = 0;
  bool began_ = false;
  bool finished_ = false;
  bool schema_matches_ = false;
};

void TestSuccessfulPipeline() {
  FakeSource source({MakeChunk({1, 2}), MakeChunk({3})});
  FakeSink sink;

  const auto result = borophene::execution::RunPipeline(source, sink);
  Check(result.has_value(), "pipeline succeeds");
  Check(sink.Began() && sink.Finished() && sink.SchemaMatches(), "sink lifecycle completes with source schema");
  Check(sink.Writes() == 2 && sink.Rows() == 3, "every chunk is written");
  Check(source.Calls() == 3, "source is read through explicit EOF");
}

void TestErrorPropagation() {
  FakeSource begin_source({MakeChunk({1})});
  FakeSink begin_sink(FakeSink::FailurePoint::kBegin);
  auto begin_result = borophene::execution::RunPipeline(begin_source, begin_sink);
  Check(!begin_result && begin_result.error().Message() == "begin failed", "begin errors propagate");
  Check(begin_source.Calls() == 0 && !begin_sink.Finished(), "begin failure stops the pipeline");

  FakeSource source_failure({}, true);
  FakeSink source_sink;
  auto source_result = borophene::execution::RunPipeline(source_failure, source_sink);
  Check(!source_result && source_result.error().Message() == "source failed", "source errors propagate");
  Check(!source_sink.Finished(), "source failure does not finish the sink");

  FakeSource write_source({MakeChunk({1})});
  FakeSink write_sink(FakeSink::FailurePoint::kWrite);
  auto write_result = borophene::execution::RunPipeline(write_source, write_sink);
  Check(!write_result && write_result.error().Message() == "write failed", "write errors propagate");
  Check(!write_sink.Finished(), "write failure does not finish the sink");

  FakeSource finish_source({});
  FakeSink finish_sink(FakeSink::FailurePoint::kFinish);
  auto finish_result = borophene::execution::RunPipeline(finish_source, finish_sink);
  Check(!finish_result && finish_result.error().Message() == "finish failed", "finish errors propagate");
}

}  // namespace

int main() {
  TestSuccessfulPipeline();
  TestErrorPropagation();
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
  }
  return failures == 0 ? 0 : 1;
}
