#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/data/data_chunk.hpp"
#include "borophene/data/schema.hpp"
#include "borophene/execution/chunk_source.hpp"
#include "borophene/io/file.hpp"
#include "borophene/storage/columnar_format.hpp"

namespace borophene::storage {

class ColumnarReader final : public execution::ChunkSource {
 public:
  static Result<ColumnarReader> Open(std::unique_ptr<io::RandomAccessFile> file);
  static Result<ColumnarReader> Open(const std::filesystem::path& path);

  ColumnarReader(const ColumnarReader&) = delete;
  ColumnarReader& operator=(const ColumnarReader&) = delete;
  ColumnarReader(ColumnarReader&&) noexcept = default;
  ColumnarReader& operator=(ColumnarReader&&) = delete;

  const Schema& GetSchema() const noexcept override;
  Index RowCount() const noexcept;
  Index RowGroupCount() const noexcept;

  Result<DataChunk> ReadRowGroup(Index index) const;
  Result<std::optional<DataChunk>> Next() override;
  void Reset() noexcept;

 private:
  ColumnarReader(std::unique_ptr<io::RandomAccessFile> file, Schema schema, std::vector<RowGroupMetadata> row_groups,
                 Index total_rows) noexcept;

  std::unique_ptr<io::RandomAccessFile> file_;
  Schema schema_;
  std::vector<RowGroupMetadata> row_groups_;
  Index total_rows_;
  Index next_row_group_ = 0;
};

}  // namespace borophene::storage
