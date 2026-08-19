#ifndef BOROPHENE_DATA_DATA_CHUNK_HPP_
#define BOROPHENE_DATA_DATA_CHUNK_HPP_

#include <utility>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"
#include "borophene/data/column_vector.hpp"
#include "borophene/data/schema.hpp"

namespace borophene {

class DataChunk {
 public:
  [[nodiscard]] static Result<DataChunk> Create(const Schema& schema, std::vector<ColumnVector> columns);

  [[nodiscard]] const std::vector<ColumnVector>& Columns() const noexcept { return columns_; }
  [[nodiscard]] const ColumnVector& Column(Index index) const;
  [[nodiscard]] Index RowCount() const noexcept { return row_count_; }

 private:
  DataChunk(std::vector<ColumnVector> columns, Index row_count) : columns_(std::move(columns)), row_count_(row_count) {}

  std::vector<ColumnVector> columns_;
  Index row_count_;
};

}  // namespace borophene

#endif  // BOROPHENE_DATA_DATA_CHUNK_HPP_
