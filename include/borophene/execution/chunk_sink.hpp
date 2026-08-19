#pragma once

#include "borophene/common/result.hpp"
#include "borophene/data/data_chunk.hpp"
#include "borophene/data/schema.hpp"

namespace borophene::execution {

class ChunkSink {
 public:
  virtual ~ChunkSink() = default;

  ChunkSink() = default;
  ChunkSink(const ChunkSink&) = delete;
  ChunkSink& operator=(const ChunkSink&) = delete;
  ChunkSink(ChunkSink&&) noexcept = default;
  ChunkSink& operator=(ChunkSink&&) noexcept = default;

  [[nodiscard]] virtual Result<void> Begin(const Schema& schema) = 0;
  [[nodiscard]] virtual Result<void> Write(const DataChunk& chunk) = 0;
  [[nodiscard]] virtual Result<void> Finish() = 0;
};

}  // namespace borophene::execution
