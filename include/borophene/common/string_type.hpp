//===----------------------------------------------------------------------===//
//                         Borophene Storage Engine
//
// This value type is based on DuckDB's string_type.hpp representation.
// Original source: https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/common/types/string_type.hpp
//
// Copyright 2018-2026 Stichting DuckDB Foundation
// Licensed under the MIT License.
//
// Modifications made for the Borophene project.
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"

namespace borophene {

// Strings longer than kInlineLength are non-owning. The referenced bytes must
// outlive this value and every copy made from it.
// TODO: Back long strings with a column-owned arena so non-owning StringT values
// have storage that matches the lifetime of their column.
class StringT {
 public:
  static constexpr Index kPrefixLength = 4;
  static constexpr Index kInlineLength = 12;
  static constexpr Index kPrefixBytes = kPrefixLength;
  static constexpr Index kInlineBytes = kInlineLength;

  constexpr StringT() noexcept : storage_{} {
  }

  static Result<StringT> Create(const char* data, Index length);
  static Result<StringT> Create(const char* data);
  static Result<StringT> Create(std::string_view value);
  static Result<StringT> Create(const std::string& value);
  static Result<StringT> Create(std::string&&) = delete;
  static Result<StringT> Create(const std::string&&) = delete;

  Index GetSize() const noexcept {
    return storage_.inlined_.length_;
  }

  const char* GetData() const noexcept {
    return IsInlined() ? storage_.inlined_.data_.data() : storage_.borrowed_.data_;
  }

  std::string GetString() const {
    std::string result;
    result.resize(GetSize());
    if (!result.empty()) {
      std::memcpy(result.data(), GetData(), result.size());
    }
    return result;
  }

  std::string_view GetView() const noexcept {
    return {GetData(), GetSize()};
  }

  bool Empty() const noexcept {
    return GetSize() == 0;
  }

  bool IsInlined() const noexcept {
    return GetSize() <= kInlineLength;
  }

  ui32 GetPrefixIntegerComparable() const noexcept {
    ui32 result = 0;
    const auto kPrefixSize = std::min(GetSize(), kPrefixLength);
    for (Index index = 0; index < kPrefixLength; ++index) {
      result <<= 8U;
      if (index < kPrefixSize) {
        result |= static_cast<unsigned char>(GetData()[index]);
      }
    }
    return result;
  }

  explicit operator std::string() const {
    return GetString();
  }

  bool operator==(const StringT& other) const noexcept {
    return GetSize() == other.GetSize() && CompareBytes(other, GetSize()) == 0;
  }

  bool operator!=(const StringT& other) const noexcept {
    return !(*this == other);
  }

  bool operator<(const StringT& other) const noexcept {
    const Index kComparedSize = std::min(GetSize(), other.GetSize());
    const int kComparison = CompareBytes(other, kComparedSize);
    return kComparison < 0 || (kComparison == 0 && GetSize() < other.GetSize());
  }

  bool operator>(const StringT& other) const noexcept {
    return other < *this;
  }

  bool operator<=(const StringT& other) const noexcept {
    return !(other < *this);
  }

  bool operator>=(const StringT& other) const noexcept {
    return !(*this < other);
  }

 private:
  struct Inlined {
    ui32 length_;
    std::array<char, kInlineLength> data_;
  };

  struct Borrowed {
    ui32 length_;
    std::array<char, kPrefixLength> prefix_;
    const char* data_;
  };

  union Storage {
    Inlined inlined_;
    Borrowed borrowed_;
  } storage_{};

  int CompareBytes(const StringT& other, Index length) const noexcept {
    if (length == 0) {
      return 0;
    }
    return std::memcmp(GetData(), other.GetData(), length);
  }

  void Initialize(const char* data, Index length) noexcept {
    const auto kStoredLength = static_cast<ui32>(length);
    if (length <= kInlineLength) {
      storage_.inlined_ = Inlined{.length_ = kStoredLength, .data_ = {}};
      if (length != 0) {
        std::memcpy(storage_.inlined_.data_.data(), data, length);
      }
      return;
    }

    std::construct_at(&storage_.borrowed_, Borrowed{.length_ = kStoredLength, .prefix_ = {}, .data_ = data});
    std::memcpy(storage_.borrowed_.prefix_.data(), data, kPrefixLength);
  }
};

inline Result<StringT> StringT::Create(const char* data, Index length) {
  if (data == nullptr && length != 0) {
    return Failure<StringT>(ErrorCode::kInvalidArgument, "StringT cannot borrow a null pointer with a nonzero length");
  }
  if (length > std::numeric_limits<ui32>::max()) {
    return Failure<StringT>(ErrorCode::kOutOfRange, "StringT length exceeds its 32-bit representation");
  }

  StringT result;
  result.Initialize(data, length);
  return result;
}

inline Result<StringT> StringT::Create(const char* data) {
  if (data == nullptr) {
    return Failure<StringT>(ErrorCode::kInvalidArgument, "StringT cannot read a null C string");
  }
  return Create(data, std::strlen(data));
}

inline Result<StringT> StringT::Create(std::string_view value) {
  return Create(value.data(), value.size());
}

inline Result<StringT> StringT::Create(const std::string& value) {
  return Create(value.data(), value.size());
}

static_assert(sizeof(StringT) == 16, "StringT must remain a compact 16-byte value");
static_assert(std::is_trivially_copyable_v<StringT>, "StringT copies must preserve their value representation");

}  // namespace borophene
