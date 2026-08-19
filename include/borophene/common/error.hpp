#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace borophene {

enum class ErrorCode : std::uint8_t {
  kInvalidArgument,
  kInvalidState,
  kIo,
  kMalformedCsv,
  kInvalidFormat,
  kUnsupportedVersion,
  kSchemaMismatch,
  kOutOfRange,
};

class Error {
 public:
  Error(ErrorCode code, std::string message);

  ErrorCode Code() const noexcept {
    return code_;
  }

  const std::string& Message() const noexcept {
    return message_;
  }

  bool operator==(const Error&) const = default;

 private:
  ErrorCode code_;
  std::string message_;
};

std::string_view ToString(ErrorCode code) noexcept;

}  // namespace borophene
