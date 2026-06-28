#pragma once

#include <cstdint>
#include <memory>

#include "allocator.h"
#include "borophene/common/types.h"
#include "borophene/common/validity_mask.h"
#include "typedefs.h"
#include "types.h"

namespace borophene {

enum class VectorType : uint8_t { kFlat, kConstant, kDictionary };

struct VectorBuffer {
  ValidityMask validity;
  DataPtr* data_ptr;
  Index type_size;
  Index capacity;
  AllocatedData allocated_data;
};

class Vector {
 private:
  LogicalTypeId logical_type_;
  std::shared_ptr<VectorBuffer> buffer_;
};

}  // namespace borophene
