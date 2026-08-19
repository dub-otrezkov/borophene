#include "borophene/common/error.hpp"

#include <utility>

namespace borophene {

Error::Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {
}

std::string_view ToString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kInvalidArgument:
      return "invalid argument";
    case ErrorCode::kInvalidState:
      return "invalid state";
    case ErrorCode::kIo:
      return "I/O error";
    case ErrorCode::kMalformedCsv:
      return "malformed CSV";
    case ErrorCode::kInvalidFormat:
      return "invalid format";
    case ErrorCode::kUnsupportedVersion:
      return "unsupported version";
    case ErrorCode::kSchemaMismatch:
      return "schema mismatch";
    case ErrorCode::kOutOfRange:
      return "out of range";
  }
  return "unknown error";
}

}  // namespace borophene
