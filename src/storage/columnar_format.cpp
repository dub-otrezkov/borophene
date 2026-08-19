#include "borophene/storage/columnar_format.hpp"

namespace borophene::storage {

const std::array<char, 8> kColumnarHeaderMagic = {'B', 'O', 'R', 'O', 'P', 'H', 'E', 'N'};
const std::array<char, 8> kColumnarTrailerMagic = {'B', 'O', 'R', 'O', '_', 'E', 'N', 'D'};

}  // namespace borophene::storage
