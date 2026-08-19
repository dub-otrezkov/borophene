#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"
#include "borophene/data/column_vector.hpp"
#include "borophene/data/schema.hpp"

namespace borophene {

class DataChunk {
 public:
  static Result<DataChunk> Create(const Schema& schema, std::vector<ColumnVector>&& columns);

  const std::vector<ColumnVector>& Columns() const noexcept {
    return columns_;
  }

  Result<std::reference_wrapper<const ColumnVector>> Column(Index index) const;
  Index RowCount() const noexcept {
    return row_count_;
  }

 private:
  DataChunk(std::vector<ColumnVector>&& columns, Index row_count)
      : columns_(std::move(columns)), row_count_(row_count) {
  }

  std::vector<ColumnVector> columns_;
  Index row_count_;
};

}  // namespace borophene
