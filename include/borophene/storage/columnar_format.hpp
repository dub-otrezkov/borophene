#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "borophene/common/types.hpp"

namespace borophene::storage {

inline constexpr ui16 kColumnarFormatMajor = 1;
inline constexpr ui16 kColumnarFormatMinor = 0;
inline constexpr ui32 kColumnarFormatFlags = 0;
inline constexpr ui64 kColumnarHeaderSize = 16;
inline constexpr ui64 kColumnarTrailerSize = 40;
inline constexpr ui32 kColumnarMaxFieldCount = 16U * 1024U;
inline constexpr ui32 kColumnarMaxRowGroupCount = 1'000'000U;
inline constexpr ui32 kColumnarMaxFieldNameSize = 1024U * 1024U;
inline constexpr ui64 kColumnarMaxMetadataSize = 256ULL * 1024ULL * 1024ULL;
inline constexpr ui64 kColumnarMaxChunkSize = 1024ULL * 1024ULL * 1024ULL;

extern const std::array<char, 8> kColumnarHeaderMagic;
extern const std::array<char, 8> kColumnarTrailerMagic;

enum class ColumnEncoding : ui8 {
  kPlain = 0,
};

enum class ColumnCompression : ui8 {
  kNone = 0,
  kZstd = 1,
};

struct ColumnChunkMetadata {
  ui64 offset = 0;
  ui64 stored_size = 0;
  ui64 decoded_size = 0;
  ui32 null_count = 0;
  ColumnEncoding encoding = ColumnEncoding::kPlain;
  ColumnCompression compression = ColumnCompression::kNone;
  ui16 flags = 0;
};

struct RowGroupMetadata {
  Index first_row = 0;
  ui32 row_count = 0;
  std::vector<ColumnChunkMetadata> chunks;
};

constexpr ui64 ValidityBitmapSize(ui64 row_count) noexcept {
  return (row_count / 8U) + (row_count % 8U != 0U ? 1U : 0U);
}

}  // namespace borophene::storage
