#pragma once

#include <cstring>

namespace borophene {

template <typename T>
T Load(const void* ptr) {
  T ret;
  memcpy(&ret, ptr, sizeof(ret));
  return ret;
}

}  // namespace borophene
