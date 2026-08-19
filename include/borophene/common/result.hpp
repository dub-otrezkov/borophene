#pragma once

#include <expected>
#include <string>
#include <utility>

#include "borophene/common/error.hpp"

namespace borophene {

template <typename T>
using Result = std::expected<T, Error>;

template <typename T>
Result<T> Failure(ErrorCode code, std::string message) {
  return std::unexpected(Error(code, std::move(message)));
}

}  // namespace borophene
