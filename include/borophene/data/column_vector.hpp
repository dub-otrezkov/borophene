#pragma once

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

  Index Size() const noexcept {
    return size_;
  }

  void SetValid(Index index, bool valid = true);
  bool IsValid(Index index) const;
  Index NullCount() const noexcept;
  const std::vector<std::uint64_t>& Words() const noexcept {
    return words_;
  }

  bool operator==(const ValidityMask&) const = default;

 private:
  void CheckIndex(Index index) const;

  Index size_;
  std::vector<std::uint64_t> words_;
};

class ColumnVector {
 public:
  static Result<ColumnVector> CreateInt32(std::vector<std::int32_t> values,
                                          std::optional<ValidityMask> validity = std::nullopt);
  static Result<ColumnVector> CreateString(std::vector<std::string> values,
                                           std::optional<ValidityMask> validity = std::nullopt);

  LogicalType Type() const noexcept {
    return type_;
  }

  Index Size() const noexcept;
  const ValidityMask& Validity() const noexcept {
    return validity_;
  }

  const std::vector<std::int32_t>& Int32Values() const;
  const std::vector<std::string>& StringValues() const;

 private:
  using Values = std::variant<std::vector<std::int32_t>, std::vector<std::string>>;

  ColumnVector(LogicalType type, Values values, ValidityMask validity)
      : type_(type), values_(std::move(values)), validity_(std::move(validity)) {
  }

  template <typename T>
  static Result<ColumnVector> Create(LogicalType type, std::vector<T> values, std::optional<ValidityMask> validity);

  LogicalType type_;
  Values values_;
  ValidityMask validity_;
};

}  // namespace borophene
