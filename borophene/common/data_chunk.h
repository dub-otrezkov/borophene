#pragma once

#include <vector>

#include "borophene/common/flat_vector.h"
#include "borophene/common/vector.h"

namespace borophene {

struct DataChunk {
  std::vector<FlatVector> vectors;
};

}  // namespace borophene
