#include "borophene/data/column_vector.hpp"

#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace borophene {
namespace {

constexpr Index kBitsPerWord = std::numeric_limits<std::uint64_t>::digits;

std::size_t WordCount(Index size) {
  const Index kWordCount = (size / kBitsPerWord) + static_cast<Index>(size % kBitsPerWord != 0);
  const auto kMaxWords = std::vector<std::uint64_t>{}.max_size();
  if (kWordCount > kMaxWords) {
    throw std::length_error("validity mask exceeds the maximum vector size");
  }
  return static_cast<std::size_t>(kWordCount);
}

}  // namespace

ValidityMask::ValidityMask(Index size)
    : size_(size), words_(WordCount(size), std::numeric_limits<std::uint64_t>::max()) {
  if (!words_.empty() && size % kBitsPerWord != 0) {
    words_.back() = (std::uint64_t{1} << (size % kBitsPerWord)) - 1;
  }
}

void ValidityMask::SetValid(Index index, bool valid) {
  CheckIndex(index);
  const std::uint64_t kBit = std::uint64_t{1} << (index % kBitsPerWord);
  if (valid) {
    words_[index / kBitsPerWord] |= kBit;
  } else {
    words_[index / kBitsPerWord] &= ~kBit;
  }
}

bool ValidityMask::IsValid(Index index) const {
  CheckIndex(index);
  const std::uint64_t kBit = std::uint64_t{1} << (index % kBitsPerWord);
  return (words_[index / kBitsPerWord] & kBit) != 0;
}

Index ValidityMask::NullCount() const noexcept {
  Index valid_count = 0;
  for (const auto kWord : words_) {
    valid_count += std::popcount(kWord);
  }
  return size_ - valid_count;
}

void ValidityMask::CheckIndex(Index index) const {
  if (index >= size_) {
    throw std::out_of_range("validity-mask index is out of range");
  }
}

template <typename T>
Result<ColumnVector> ColumnVector::Create(LogicalType type, std::vector<T> values,
                                          std::optional<ValidityMask> validity) {
  if (validity.has_value() && validity->Size() != values.size()) {
    return Failure<ColumnVector>(ErrorCode::kInvalidArgument,
                                 "validity-mask size must equal the number of column values");
  }
  ValidityMask final_validity = validity.has_value() ? std::move(*validity) : ValidityMask(values.size());
  return ColumnVector(type, Values(std::move(values)), std::move(final_validity));
}

Result<ColumnVector> ColumnVector::CreateInt32(std::vector<std::int32_t> values, std::optional<ValidityMask> validity) {
  return Create(LogicalType::kInt32, std::move(values), std::move(validity));
}

Result<ColumnVector> ColumnVector::CreateString(std::vector<std::string> values, std::optional<ValidityMask> validity) {
  return Create(LogicalType::kString, std::move(values), std::move(validity));
}

Index ColumnVector::Size() const noexcept {
  return validity_.Size();
}

const std::vector<std::int32_t>& ColumnVector::Int32Values() const {
  if (type_ != LogicalType::kInt32) {
    throw std::logic_error("column does not contain INT32 values");
  }
  return std::get<std::vector<std::int32_t>>(values_);
}

const std::vector<std::string>& ColumnVector::StringValues() const {
  if (type_ != LogicalType::kString) {
    throw std::logic_error("column does not contain STRING values");
  }
  return std::get<std::vector<std::string>>(values_);
}

}  // namespace borophene
