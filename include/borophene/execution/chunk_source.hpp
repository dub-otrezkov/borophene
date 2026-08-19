#pragma once

#include <optional>

#include "borophene/common/result.hpp"
#include "borophene/data/data_chunk.hpp"
#include "borophene/data/schema.hpp"

namespace borophene::execution {

class ChunkSource {
 public:
  virtual ~ChunkSource() = default;

  ChunkSource() = default;
  ChunkSource(const ChunkSource&) = delete;
  ChunkSource& operator=(const ChunkSource&) = delete;
  ChunkSource(ChunkSource&&) noexcept = default;
  ChunkSource& operator=(ChunkSource&&) noexcept = default;

  [[nodiscard]] virtual const Schema& GetSchema() const noexcept = 0;
  [[nodiscard]] virtual Result<std::optional<DataChunk>> Next() = 0;
};

}  // namespace borophene::execution
