#pragma once

#include <expected>
#include <string>
#include <utility>

#include "borophene/common/error.hpp"

namespace borophene {

template <typename T>
using Result = std::expected<T, Error>;

using Unexpected = std::unexpected<Error>;

inline Unexpected MakeUnexpected(Error error) {
  return Unexpected(std::move(error));
}

template <typename T>
Result<T> Failure(ErrorCode code, std::string message) {
  return MakeUnexpected(Error(code, std::move(message)));
}

}  // namespace borophene
