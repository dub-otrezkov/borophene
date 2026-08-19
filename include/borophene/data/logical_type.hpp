#ifndef BOROPHENE_DATA_LOGICAL_TYPE_HPP_
#define BOROPHENE_DATA_LOGICAL_TYPE_HPP_

#include <cstdint>
#include <string_view>

#include "borophene/common/result.hpp"

namespace borophene {

enum class LogicalType : std::uint8_t {
  kInt32 = 1,
  kString = 2,
};

[[nodiscard]] constexpr bool IsSupportedLogicalType(LogicalType type) noexcept {
  switch (type) {
    case LogicalType::kInt32:
    case LogicalType::kString:
      return true;
  }
  return false;
}

[[nodiscard]] std::string_view ToString(LogicalType type) noexcept;
[[nodiscard]] Result<LogicalType> ParseLogicalType(std::string_view text);

}  // namespace borophene

#endif  // BOROPHENE_DATA_LOGICAL_TYPE_HPP_
