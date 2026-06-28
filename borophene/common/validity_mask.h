#pragma once

#include <cstdint>
#include <vector>

#include "borophene/common/types.h"

namespace borophene {

class ValidityMask {
 public:
  using Word = uint64_t;

  bool IsValid(Index index);

 private:
  std::vector<Word> data_;
};

}  // namespace borophene
