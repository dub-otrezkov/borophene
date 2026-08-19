#ifndef BOROPHENE_STORAGE_COLUMNAR_FORMAT_HPP_
#define BOROPHENE_STORAGE_COLUMNAR_FORMAT_HPP_

#include <array>
#include <cstdint>
#include <vector>

#include "borophene/common/types.hpp"

namespace borophene::storage {

inline constexpr std::uint16_t kColumnarFormatMajor = 1;
inline constexpr std::uint16_t kColumnarFormatMinor = 0;
inline constexpr std::uint32_t kColumnarFormatFlags = 0;
inline constexpr std::uint64_t kColumnarHeaderSize = 16;
inline constexpr std::uint64_t kColumnarTrailerSize = 40;
inline constexpr std::uint32_t kColumnarMaxFieldCount = 16U * 1024U;
inline constexpr std::uint32_t kColumnarMaxRowGroupCount = 1'000'000U;
inline constexpr std::uint32_t kColumnarMaxFieldNameSize = 1024U * 1024U;
inline constexpr std::uint64_t kColumnarMaxMetadataSize = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kColumnarMaxChunkSize = 1024ULL * 1024ULL * 1024ULL;

extern const std::array<char, 8> kColumnarHeaderMagic;
extern const std::array<char, 8> kColumnarTrailerMagic;

enum class ColumnEncoding : std::uint8_t {
  kPlain = 0,
};

enum class ColumnCompression : std::uint8_t {
  kNone = 0,
};

struct ColumnChunkMetadata {
  std::uint64_t offset = 0;
  std::uint64_t stored_size = 0;
  std::uint64_t decoded_size = 0;
  std::uint32_t null_count = 0;
  ColumnEncoding encoding = ColumnEncoding::kPlain;
  ColumnCompression compression = ColumnCompression::kNone;
  std::uint16_t flags = 0;
};

struct RowGroupMetadata {
  Index first_row = 0;
  std::uint32_t row_count = 0;
  std::vector<ColumnChunkMetadata> chunks;
};

[[nodiscard]] constexpr std::uint64_t ValidityBitmapSize(std::uint64_t row_count) noexcept {
  return (row_count / 8U) + (row_count % 8U != 0U ? 1U : 0U);
}

}  // namespace borophene::storage

#endif  // BOROPHENE_STORAGE_COLUMNAR_FORMAT_HPP_
