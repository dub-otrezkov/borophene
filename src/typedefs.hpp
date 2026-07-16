#pragma once

#include <cstdint>

namespace borophene {

static_assert(std::endian::native == std::endian::little, "That engine supports only Little-Endian architectures");

using idx_t = uint64_t;

//! data pointers
using data_t = uint8_t;
using data_ptr_t = data_t*;
using const_data_ptr_t = const data_t*;

}  // namespace borophene
