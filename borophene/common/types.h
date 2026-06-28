#pragma once

#include <cstdint>

namespace borophene {

using Index = uint64_t;

enum class PhysicalType : uint8_t {
  kBool = 1,
  kInt8 = 2,
  kInt16 = 3,
  kInt32 = 4,
  kInt64 = 5,
  kFloat = 6,
  kDouble = 7,
};

enum class LogicalTypeId : uint8_t {
  kBool = 1,
  kInt8 = 2,
  kInt16 = 3,
  kInt32 = 4,
  kInt64 = 5,
  kFloat = 6,
  kDouble = 7,
  kDate = 8,
  kTimestamp = 9,
};

}  // namespace borophene
