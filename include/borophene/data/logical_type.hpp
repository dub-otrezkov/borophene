#pragma once

#include <string_view>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"

namespace borophene {

enum class LogicalType : ui8 {
  kInt32 = 1,
  kString = 2,
};

constexpr bool IsSupportedLogicalType(LogicalType type) noexcept {
  switch (type) {
    case LogicalType::kInt32:
    case LogicalType::kString:
      return true;
  }
  return false;
}

std::string_view ToString(LogicalType type) noexcept;
Result<LogicalType> ParseLogicalType(std::string_view text);

}  // namespace borophene
