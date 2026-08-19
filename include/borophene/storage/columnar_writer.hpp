#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/data/data_chunk.hpp"
#include "borophene/data/schema.hpp"
#include "borophene/execution/chunk_sink.hpp"
#include "borophene/io/file.hpp"
#include "borophene/storage/columnar_format.hpp"

namespace borophene::storage {

class ColumnarWriter final : public execution::ChunkSink {
 public:
  static Result<ColumnarWriter> Create(std::unique_ptr<io::OutputFile> file);
  static Result<ColumnarWriter> Create(const std::filesystem::path& path);

  ColumnarWriter(const ColumnarWriter&) = delete;
  ColumnarWriter& operator=(const ColumnarWriter&) = delete;
  ColumnarWriter(ColumnarWriter&&) noexcept = default;
  ColumnarWriter& operator=(ColumnarWriter&&) noexcept = default;

  Result<void> Begin(const Schema& schema) override;
  Result<void> Write(const DataChunk& chunk) override;
  Result<void> WriteRowGroup(const DataChunk& chunk);
  Result<void> Finish() override;

 private:
  enum class State {
    kCreated,
    kBegun,
    kFinished,
    kFailed,
  };

  explicit ColumnarWriter(std::unique_ptr<io::OutputFile> file) noexcept;

  std::unique_ptr<io::OutputFile> file_;
  std::optional<Schema> schema_;
  std::vector<RowGroupMetadata> row_groups_;
  Index total_rows_ = 0;
  State state_ = State::kCreated;
};

}  // namespace borophene::storage
