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
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "borophene/common/types.hpp"

namespace borophene {

// Strings longer than kInlineLength are non-owning. The referenced bytes must
// outlive this value and every copy made from it.
class StringT {
 public:
  static constexpr Index kPrefixLength = 4;
  static constexpr Index kInlineLength = 12;
  static constexpr Index kPrefixBytes = kPrefixLength;
  static constexpr Index kInlineBytes = kInlineLength;

  constexpr StringT() noexcept : storage_{} {
  }

  StringT(const char* data, Index length) {
    Initialize(data, length);
  }

  StringT(const char* data) {  // NOLINT: Preserves string-literal ergonomics.
    if (data == nullptr) {
      throw std::invalid_argument("StringT cannot read a null C string");
    }
    Initialize(data, std::strlen(data));
  }

  StringT(std::string_view value)  // NOLINT: StringT is a string-view value.
      : StringT(value.data(), value.size()) {
  }

  StringT(const std::string& value)  // NOLINT: Long values are explicitly borrowed.
      : StringT(value.data(), value.size()) {
  }

  StringT(std::string&&) = delete;
  StringT(const std::string&&) = delete;

  Index GetSize() const noexcept {
    return storage_.inlined_.length_;
  }

  const char* GetData() const noexcept {
    return IsInlined() ? storage_.inlined_.data_.data() : storage_.borrowed_.data_;
  }

  std::string GetString() const {
    return std::string(GetView());
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

  std::uint32_t GetPrefixIntegerComparable() const noexcept {
    std::uint32_t result = 0;
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
    return GetView() == other.GetView();
  }

  bool operator!=(const StringT& other) const noexcept {
    return !(*this == other);
  }

  bool operator<(const StringT& other) const noexcept {
    return GetView() < other.GetView();
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
    std::uint32_t length_;
    std::array<char, kInlineLength> data_;
  };

  struct Borrowed {
    std::uint32_t length_;
    std::array<char, kPrefixLength> prefix_;
    const char* data_;
  };

  union Storage {
    Inlined inlined_;
    Borrowed borrowed_;
  } storage_{};

  void Initialize(const char* data, Index length) {
    if (data == nullptr && length != 0) {
      throw std::invalid_argument("StringT cannot borrow a null pointer with a nonzero length");
    }
    if (length > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("StringT length exceeds its 32-bit representation");
    }

    const auto kStoredLength = static_cast<std::uint32_t>(length);
    if (length <= kInlineLength) {
      storage_.inlined_ = Inlined{.length_ = kStoredLength, .data_ = {}};
      if (length != 0) {
        std::memcpy(storage_.inlined_.data_.data(), data, length);
      }
      return;
    }

    storage_.borrowed_ = Borrowed{.length_ = kStoredLength, .prefix_ = {}, .data_ = data};
    std::memcpy(storage_.borrowed_.prefix_.data(), data, kPrefixLength);
  }
};

static_assert(sizeof(StringT) == 16, "StringT must remain a compact 16-byte value");
static_assert(std::is_trivially_copyable_v<StringT>, "StringT copies must preserve their value representation");

}  // namespace borophene
