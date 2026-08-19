#include "borophene/data/data_chunk.hpp"

#include <utility>

namespace borophene {

Result<DataChunk> DataChunk::Create(const Schema& schema, std::vector<ColumnVector>&& columns) {
  if (columns.size() != schema.Size()) {
    return Failure<DataChunk>(ErrorCode::kSchemaMismatch, "data-chunk column count does not match the schema");
  }

  const Index kRowCount = columns.empty() ? 0 : columns.front().Size();
  for (Index index = 0; index < columns.size(); ++index) {
    auto field = schema.FieldAt(index);
    if (!field) {
      return MakeUnexpected(std::move(field.error()));
    }
    const Field& schema_field = field->get();
    if (columns[index].Type() != schema_field.type) {
      return Failure<DataChunk>(ErrorCode::kSchemaMismatch,
                                "data-chunk column type does not match field: " + schema_field.name);
    }
    if (columns[index].Size() != kRowCount) {
      return Failure<DataChunk>(ErrorCode::kInvalidArgument,
                                "all data-chunk columns must contain the same number of rows");
    }
    if (!schema_field.nullable && columns[index].Validity().NullCount() != 0) {
      return Failure<DataChunk>(ErrorCode::kSchemaMismatch,
                                "non-nullable field contains null values: " + schema_field.name);
    }
  }

  return DataChunk(std::move(columns), kRowCount);
}

Result<std::reference_wrapper<const ColumnVector>> DataChunk::Column(Index index) const {
  if (index >= columns_.size()) {
    return Failure<std::reference_wrapper<const ColumnVector>>(ErrorCode::kOutOfRange,
                                                               "data-chunk column index is out of range");
  }
  return std::cref(columns_[index]);
}

}  // namespace borophene
