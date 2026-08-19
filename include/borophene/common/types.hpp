#ifndef BOROPHENE_COMMON_TYPES_HPP_
#define BOROPHENE_COMMON_TYPES_HPP_

#include <cstddef>
#include <cstdint>

namespace borophene {

using Index = std::uint64_t;
using Byte = std::byte;
using BytePointer = Byte*;
using ConstBytePointer = const Byte*;

}  // namespace borophene

#endif  // BOROPHENE_COMMON_TYPES_HPP_
