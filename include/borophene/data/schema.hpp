#ifndef BOROPHENE_DATA_SCHEMA_HPP_
#define BOROPHENE_DATA_SCHEMA_HPP_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"
#include "borophene/data/logical_type.hpp"

namespace borophene {

struct Field {
  std::string name;       // NOLINT(readability-identifier-naming)
  LogicalType type;       // NOLINT(readability-identifier-naming)
  bool nullable = false;  // NOLINT(readability-identifier-naming)

  bool operator==(const Field&) const = default;
};

class Schema {
 public:
  [[nodiscard]] static Result<Schema> Create(std::vector<Field> fields);

  [[nodiscard]] const std::vector<Field>& Fields() const noexcept { return fields_; }
  [[nodiscard]] Index Size() const noexcept { return fields_.size(); }
  [[nodiscard]] const Field& operator[](Index index) const;
  [[nodiscard]] Result<Index> Find(std::string_view name) const;

  bool operator==(const Schema&) const = default;

 private:
  explicit Schema(std::vector<Field> fields) : fields_(std::move(fields)) {}

  std::vector<Field> fields_;
};

}  // namespace borophene

#endif  // BOROPHENE_DATA_SCHEMA_HPP_
