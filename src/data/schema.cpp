#include "borophene/data/schema.hpp"

#include <unordered_set>
#include <utility>

namespace borophene {

Result<Schema> Schema::Create(std::vector<Field>&& fields) {
  if (fields.empty()) {
    return Failure<Schema>(ErrorCode::kInvalidArgument, "a schema must contain at least one field");
  }

  std::unordered_set<std::string_view> names;
  names.reserve(fields.size());
  for (const auto& field : fields) {
    if (!IsSupportedLogicalType(field.type)) {
      return Failure<Schema>(ErrorCode::kInvalidArgument, "field has an unsupported logical type: " + field.name);
    }
    if (field.name.empty()) {
      return Failure<Schema>(ErrorCode::kInvalidArgument, "field names cannot be empty");
    }
    if (!names.insert(field.name).second) {
      return Failure<Schema>(ErrorCode::kInvalidArgument, "duplicate field name: " + field.name);
    }
  }

  return Schema(std::move(fields));
}

Result<std::reference_wrapper<const Field>> Schema::FieldAt(Index index) const {
  if (index >= fields_.size()) {
    return Failure<std::reference_wrapper<const Field>>(ErrorCode::kOutOfRange, "schema field index is out of range");
  }
  return std::cref(fields_[index]);
}

Result<Index> Schema::Find(std::string_view name) const {
  for (Index index = 0; index < fields_.size(); ++index) {
    if (fields_[index].name == name) {
      return index;
    }
  }
  return Failure<Index>(ErrorCode::kOutOfRange, "schema has no field named: " + std::string(name));
}

}  // namespace borophene
