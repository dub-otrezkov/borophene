#pragma once

#include <functional>
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
  static Result<ValidityMask> Create(Index size);

  Index Size() const noexcept {
    return size_;
  }

  Result<void> SetValid(Index index, bool valid = true);
  Result<bool> IsValid(Index index) const;
  Index NullCount() const noexcept;
  const std::vector<ui64>& Words() const noexcept {
    return words_;
  }

  bool operator==(const ValidityMask&) const = default;

 private:
  ValidityMask(Index size, std::size_t word_count);

  Index size_;
  std::vector<ui64> words_;
};

class ColumnVector {
 public:
  static Result<ColumnVector> CreateInt32(std::vector<i32>&& values,
                                          std::optional<ValidityMask>&& validity = std::nullopt);
  static Result<ColumnVector> CreateString(std::vector<std::string>&& values,
                                           std::optional<ValidityMask>&& validity = std::nullopt);

  LogicalType Type() const noexcept {
    return type_;
  }

  Index Size() const noexcept;
  const ValidityMask& Validity() const noexcept {
    return validity_;
  }

  Result<std::reference_wrapper<const std::vector<i32>>> Int32Values() const;
  Result<std::reference_wrapper<const std::vector<std::string>>> StringValues() const;

 private:
  using Values = std::variant<std::vector<i32>, std::vector<std::string>>;

  ColumnVector(LogicalType type, Values&& values, ValidityMask&& validity)
      : type_(type), values_(std::move(values)), validity_(std::move(validity)) {
  }

  template <typename T>
  static Result<ColumnVector> Create(LogicalType type, std::vector<T>&& values, std::optional<ValidityMask>&& validity);

  LogicalType type_;
  Values values_;
  ValidityMask validity_;
};

}  // namespace borophene
