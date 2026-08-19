#include "borophene/data/logical_type.hpp"

#include <string>

namespace borophene {

std::string_view ToString(LogicalType type) noexcept {
  switch (type) {
    case LogicalType::kInt32:
      return "INT32";
    case LogicalType::kString:
      return "STRING";
  }
  return "UNKNOWN";
}

Result<LogicalType> ParseLogicalType(std::string_view text) {
  if (text == "INT32" || text == "int32") {
    return LogicalType::kInt32;
  }
  if (text == "STRING" || text == "string") {
    return LogicalType::kString;
  }
  return Failure<LogicalType>(ErrorCode::kInvalidArgument, "unknown logical type: " + std::string(text));
}

}  // namespace borophene
