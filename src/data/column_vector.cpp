#include "borophene/data/column_vector.hpp"

#include <bit>
#include <limits>
#include <utility>

namespace borophene {
namespace {

constexpr Index kBitsPerWord = std::numeric_limits<ui64>::digits;

Result<std::size_t> WordCount(Index size) {
  const Index kWordCount = (size / kBitsPerWord) + static_cast<Index>(size % kBitsPerWord != 0);
  const auto kMaxWords = std::vector<ui64>{}.max_size();
  if (kWordCount > kMaxWords) {
    return Failure<std::size_t>(ErrorCode::kOutOfRange, "validity mask exceeds the maximum vector size");
  }
  return static_cast<std::size_t>(kWordCount);
}

}  // namespace

ValidityMask::ValidityMask(Index size, std::size_t word_count)
    : size_(size), words_(word_count, std::numeric_limits<ui64>::max()) {
  if (!words_.empty() && size % kBitsPerWord != 0) {
    words_.back() = (ui64{1} << (size % kBitsPerWord)) - 1;
  }
}

Result<ValidityMask> ValidityMask::Create(Index size) {
  auto word_count = WordCount(size);
  if (!word_count) {
    return MakeUnexpected(std::move(word_count.error()));
  }
  return ValidityMask(size, *word_count);
}

Result<void> ValidityMask::SetValid(Index index, bool valid) {
  if (index >= size_) {
    return Failure<void>(ErrorCode::kOutOfRange, "validity-mask index is out of range");
  }
  const ui64 kBit = ui64{1} << (index % kBitsPerWord);
  if (valid) {
    words_[index / kBitsPerWord] |= kBit;
  } else {
    words_[index / kBitsPerWord] &= ~kBit;
  }
  return {};
}

Result<bool> ValidityMask::IsValid(Index index) const {
  if (index >= size_) {
    return Failure<bool>(ErrorCode::kOutOfRange, "validity-mask index is out of range");
  }
  const ui64 kBit = ui64{1} << (index % kBitsPerWord);
  return (words_[index / kBitsPerWord] & kBit) != 0;
}

Index ValidityMask::NullCount() const noexcept {
  Index valid_count = 0;
  for (const auto kWord : words_) {
    valid_count += std::popcount(kWord);
  }
  return size_ - valid_count;
}

template <typename T>
Result<ColumnVector> ColumnVector::Create(LogicalType type, std::vector<T>&& values,
                                          std::optional<ValidityMask>&& validity) {
  if (validity.has_value() && validity->Size() != values.size()) {
    return Failure<ColumnVector>(ErrorCode::kInvalidArgument,
                                 "validity-mask size must equal the number of column values");
  }

  auto final_validity =
      validity.has_value() ? Result<ValidityMask>(std::move(*validity)) : ValidityMask::Create(values.size());
  if (!final_validity) {
    return MakeUnexpected(std::move(final_validity.error()));
  }
  return ColumnVector(type, Values(std::move(values)), std::move(*final_validity));
}

Result<ColumnVector> ColumnVector::CreateInt32(std::vector<i32>&& values, std::optional<ValidityMask>&& validity) {
  return Create(LogicalType::kInt32, std::move(values), std::move(validity));
}

Result<ColumnVector> ColumnVector::CreateString(std::vector<std::string>&& values,
                                                std::optional<ValidityMask>&& validity) {
  return Create(LogicalType::kString, std::move(values), std::move(validity));
}

Index ColumnVector::Size() const noexcept {
  return validity_.Size();
}

Result<std::reference_wrapper<const std::vector<i32>>> ColumnVector::Int32Values() const {
  const auto* values = std::get_if<std::vector<i32>>(&values_);
  if (type_ != LogicalType::kInt32 || values == nullptr) {
    return Failure<std::reference_wrapper<const std::vector<i32>>>(ErrorCode::kInvalidState,
                                                                   "column does not contain INT32 values");
  }
  return std::cref(*values);
}

Result<std::reference_wrapper<const std::vector<std::string>>> ColumnVector::StringValues() const {
  const auto* values = std::get_if<std::vector<std::string>>(&values_);
  if (type_ != LogicalType::kString || values == nullptr) {
    return Failure<std::reference_wrapper<const std::vector<std::string>>>(ErrorCode::kInvalidState,
                                                                           "column does not contain STRING values");
  }
  return std::cref(*values);
}

}  // namespace borophene
