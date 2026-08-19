#pragma once

#include "borophene/common/result.hpp"
#include "borophene/execution/chunk_sink.hpp"
#include "borophene/execution/chunk_source.hpp"

namespace borophene::execution {

[[nodiscard]] Result<void> RunPipeline(ChunkSource& source, ChunkSink& sink);

}  // namespace borophene::execution
