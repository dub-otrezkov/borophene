#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"
#include "borophene/data/column_vector.hpp"
#include "borophene/data/schema.hpp"
#include "borophene/storage/columnar_format.hpp"

namespace borophene::storage {

struct ColumnCodecOptions {
  ColumnEncoding encoding = ColumnEncoding::kPlain;
  ColumnCompression compression = ColumnCompression::kNone;
};

struct SerializedColumn {
  std::vector<Byte> bytes;
  ui64 decoded_size = 0;
  ui32 null_count = 0;
  ColumnEncoding encoding = ColumnEncoding::kPlain;
  ColumnCompression compression = ColumnCompression::kNone;
};

struct ColumnDecodeOptions {
  ui64 decoded_size = 0;
  ui32 null_count = 0;
  ColumnEncoding encoding = ColumnEncoding::kPlain;
  ColumnCompression compression = ColumnCompression::kNone;
};

class ColumnEncoder {
 public:
  ColumnEncoder() = default;
  virtual ~ColumnEncoder() = default;

  ColumnEncoder(const ColumnEncoder&) = delete;
  ColumnEncoder& operator=(const ColumnEncoder&) = delete;
  ColumnEncoder(ColumnEncoder&&) noexcept = delete;
  ColumnEncoder& operator=(ColumnEncoder&&) noexcept = delete;

  virtual Result<SerializedColumn> Serialize(const ColumnVector& column, const Field& field) const = 0;
};

class ColumnFactory {
 public:
  ColumnFactory() = default;
  virtual ~ColumnFactory() = default;

  ColumnFactory(const ColumnFactory&) = delete;
  ColumnFactory& operator=(const ColumnFactory&) = delete;
  ColumnFactory(ColumnFactory&&) noexcept = delete;
  ColumnFactory& operator=(ColumnFactory&&) noexcept = delete;

  virtual Result<ColumnVector> Create(std::span<const Byte> bytes, const Field& field, Index row_count,
                                      const ColumnDecodeOptions& options) const = 0;
};

std::shared_ptr<const ColumnEncoder> CreateV1ColumnEncoder(const ColumnCodecOptions& options = {});
std::shared_ptr<const ColumnFactory> CreateV1ColumnFactory();

}  // namespace borophene::storage
