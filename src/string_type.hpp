//===----------------------------------------------------------------------===//
//                         Borophene Storage Engine
//
// This file is heavily based on DuckDB's string_type.hpp.
// Original source: https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/common/types/string_type.hpp
//
// Copyright (c) 2018-2026 DuckDB Labs
// Licensed under the MIT License.
//
// Modifications made for the Borophene project.
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#include "assert.hpp"
#include "helper.hpp"
#include "typedefs.hpp"

namespace borophene {

struct StringT {
  friend struct StringComparisonOperators;

 public:
  static constexpr idx_t kPrefixBytes = 4 * sizeof(char);
  static constexpr idx_t kInlineBytes = 12 * sizeof(char);
  static constexpr idx_t kPrefixLength = kPrefixBytes;
  static constexpr idx_t kInlineLength = kInlineBytes;

  StringT() = default;
  explicit StringT(uint32_t len) {
    value_.inlined_.length_ = len;
    memset(value_.inlined_.inlined_, 0, kInlineBytes);
  }
  StringT(const char* data, uint32_t len) {
    value_.inlined_.length_ = len;
    ASSERT(data || GetSize() == 0);
    if (IsInlined()) {
      // zero initialize the prefix first
      // this makes sure that strings with length smaller than 4 still have an equal prefix
      memset(value_.inlined_.inlined_, 0, kInlineBytes);
      if (GetSize() == 0) {
        return;
      }
      // small string: inlined
      memcpy(value_.inlined_.inlined_, data, GetSize());
    } else {
      // large string: store pointer
      memcpy(value_.pointer_.prefix_, data, kPrefixLength);
      value_.pointer_.ptr_ = (char*)data;  // NOLINT
    }
  }

  StringT(const char* data)  // NOLINT: Allow implicit conversion from `const char*`
      : StringT(data, static_cast<uint32_t>(strlen(data))) {}
  StringT(const std::string& value)  // NOLINT: Allow implicit conversion from `const char*`
      : StringT(value.c_str(), static_cast<uint32_t>(value.size())) {}

  bool IsInlined() const { return GetSize() <= kInlineLength; }

  const char* GetData() const {
    return IsInlined() ? reinterpret_cast<const char*>(value_.inlined_.inlined_) : value_.pointer_.ptr_;
  }
  char* GetDataWriteable() const {
    return IsInlined() ? (char*)value_.inlined_.inlined_ : value_.pointer_.ptr_;  // NOLINT
  }

  const char* GetPrefix() const { return value_.inlined_.inlined_; }
  char* GetPrefixWriteable() { return value_.inlined_.inlined_; }

  uint32_t GetPrefixIntegerComparable() const {
    return std::byteswap(Load<uint32_t>(reinterpret_cast<const_data_ptr_t>(GetPrefix())));
  }

  idx_t GetSize() const { return value_.inlined_.length_; }

  void SetSizeAndFinalize(uint32_t size, idx_t allocated_size) {
    value_.inlined_.length_ = size;
    if (allocated_size > kInlineLength && IsInlined()) {
      //! Data was written to the 'value.pointer.ptr', has to be copied to the inlined bytes
      ASSERT(value_.pointer_.ptr_);
      const char* ptr = value_.pointer_.ptr_;
      memcpy(GetDataWriteable(), ptr, size);
    }
    Finalize();
  }

  bool Empty() const { return value_.inlined_.length_ == 0; }

  std::string GetString() const { return std::string{GetData(), GetSize()}; }
  explicit operator std::string() const { return GetString(); }

  char* GetPointer() const {
    ASSERT(!IsInlined());
    return value_.pointer_.ptr_;
  }

  void SetPointer(char* new_ptr) {
    ASSERT(!IsInlined());
    value_.pointer_.ptr_ = new_ptr;
  }

  void Finalize() {
    // set trailing NULL byte
    if (GetSize() <= kInlineLength) {
      // fill prefix with zeros if the length is smaller than the prefix length
      memset(value_.inlined_.inlined_ + GetSize(), 0, kInlineBytes - GetSize());
    } else {
      // copy the data into the prefix
      const char* dataptr = GetData();
      memcpy(value_.pointer_.prefix_, dataptr, kPrefixLength);
    }
  }

  struct StringComparisonOperators {
    static bool Equals(const StringT& a, const StringT& b) {
      auto a_bulk_comp = Load<uint64_t>(&a);
      auto b_bulk_comp = Load<uint64_t>(&b);
      if (a_bulk_comp != b_bulk_comp) {
        // Either length or prefix are different -> not equal
        return false;
      }
      // they have the same length and same prefix!
      a_bulk_comp = Load<uint64_t>(&a + 8U);
      b_bulk_comp = Load<uint64_t>(&b + 8U);
      if (a_bulk_comp == b_bulk_comp) {
        // either they are both inlined (so compare equal) or point to the same string (so compare equal)
        return true;
      }
      if (!a.IsInlined()) {
        // 'long' strings of the same length -> compare pointed value
        if (memcmp(a.value_.pointer_.ptr_, b.value_.pointer_.ptr_, a.GetSize()) == 0) {
          return true;
        }
      }
      // either they are short string of same length but different content
      //     or they point to string with different content
      //     either way, they can't represent the same underlying string
      return false;
    }
    // compare up to shared length. if still the same, compare lengths
    static bool GreaterThan(const StringT& left, const StringT& right) {
      const auto kLeftLength = static_cast<uint32_t>(left.GetSize());
      const auto kRightLength = static_cast<uint32_t>(right.GetSize());
      const uint32_t kMinLength = std::min<uint32_t>(kLeftLength, kRightLength);

      auto a_prefix = Load<uint32_t>(left.GetPrefix());
      auto b_prefix = Load<uint32_t>(right.GetPrefix());

      // Check on prefix -----
      // We don't need to mask since:
      //	if the prefix is greater(after bswap), it will stay greater regardless of the extra bytes
      // 	if the prefix is smaller(after bswap), it will stay smaller regardless of the extra bytes
      //	if the prefix is equal, the extra bytes are guaranteed to be /0 for the shorter one

      if (a_prefix != b_prefix) {
        return std::byteswap(a_prefix) > std::byteswap(b_prefix);
      }

      const int kMemcmpRes = memcmp(left.GetData(), right.GetData(), kMinLength);
      return kMemcmpRes > 0 || (kMemcmpRes == 0 && kLeftLength > kRightLength);
    }
  };

  bool operator==(const StringT& r) const { return StringComparisonOperators::Equals(*this, r); }

  bool operator!=(const StringT& r) const { return !(*this == r); }

  bool operator>(const StringT& r) const { return StringComparisonOperators::GreaterThan(*this, r); }
  bool operator<(const StringT& r) const { return r > *this; }
  bool operator<=(const StringT& r) const { return r >= *this; }
  bool operator>=(const StringT& r) const { return !(*this < r); }

 private:
  union {
    struct {
      uint32_t length_;
      char prefix_[4];
      char* ptr_;
    } pointer_;
    struct {
      uint32_t length_;
      char inlined_[12];
    } inlined_;
  } value_{};
};

}  // namespace borophene
