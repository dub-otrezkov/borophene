#include "borophene/data/data_chunk.hpp"

#include <stdexcept>
#include <utility>

namespace borophene {

Result<DataChunk> DataChunk::Create(const Schema& schema, std::vector<ColumnVector> columns) {
  if (columns.size() != schema.Size()) {
    return Failure<DataChunk>(ErrorCode::kSchemaMismatch, "data-chunk column count does not match the schema");
  }

  const Index kRowCount = columns.empty() ? 0 : columns.front().Size();
  for (Index index = 0; index < columns.size(); ++index) {
    if (columns[index].Type() != schema[index].type) {
      return Failure<DataChunk>(ErrorCode::kSchemaMismatch,
                                "data-chunk column type does not match field: " + schema[index].name);
    }
    if (columns[index].Size() != kRowCount) {
      return Failure<DataChunk>(ErrorCode::kInvalidArgument,
                                "all data-chunk columns must contain the same number of rows");
    }
    if (!schema[index].nullable && columns[index].Validity().NullCount() != 0) {
      return Failure<DataChunk>(ErrorCode::kSchemaMismatch,
                                "non-nullable field contains null values: " + schema[index].name);
    }
  }

  return DataChunk(std::move(columns), kRowCount);
}

const ColumnVector& DataChunk::Column(Index index) const {
  if (index >= columns_.size()) {
    throw std::out_of_range("data-chunk column index is out of range");
  }
  return columns_[index];
}

}  // namespace borophene
