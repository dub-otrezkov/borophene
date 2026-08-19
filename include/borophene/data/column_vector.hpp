#ifndef BOROPHENE_DATA_COLUMN_VECTOR_HPP_
#define BOROPHENE_DATA_COLUMN_VECTOR_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"
#include "borophene/data/logical_type.hpp"

namespace borophene {

class ValidityMask {
 public:
  explicit ValidityMask(Index size);

  [[nodiscard]] Index Size() const noexcept { return size_; }
  void SetValid(Index index, bool valid = true);
  [[nodiscard]] bool IsValid(Index index) const;
  [[nodiscard]] Index NullCount() const noexcept;
  [[nodiscard]] const std::vector<std::uint64_t>& Words() const noexcept { return words_; }

  bool operator==(const ValidityMask&) const = default;

 private:
  void CheckIndex(Index index) const;

  Index size_;
  std::vector<std::uint64_t> words_;
};

class ColumnVector {
 public:
  [[nodiscard]] static Result<ColumnVector> CreateInt32(std::vector<std::int32_t> values,
                                                        std::optional<ValidityMask> validity = std::nullopt);
  [[nodiscard]] static Result<ColumnVector> CreateString(std::vector<std::string> values,
                                                         std::optional<ValidityMask> validity = std::nullopt);

  [[nodiscard]] LogicalType Type() const noexcept { return type_; }
  [[nodiscard]] Index Size() const noexcept;
  [[nodiscard]] const ValidityMask& Validity() const noexcept { return validity_; }
  [[nodiscard]] const std::vector<std::int32_t>& Int32Values() const;
  [[nodiscard]] const std::vector<std::string>& StringValues() const;

 private:
  using Values = std::variant<std::vector<std::int32_t>, std::vector<std::string>>;

  ColumnVector(LogicalType type, Values values, ValidityMask validity)
      : type_(type), values_(std::move(values)), validity_(std::move(validity)) {}

  template <typename T>
  [[nodiscard]] static Result<ColumnVector> Create(LogicalType type, std::vector<T> values,
                                                   std::optional<ValidityMask> validity);

  LogicalType type_;
  Values values_;
  ValidityMask validity_;
};

}  // namespace borophene

#endif  // BOROPHENE_DATA_COLUMN_VECTOR_HPP_
