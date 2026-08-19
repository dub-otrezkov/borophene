#include "borophene/execution/pipeline.hpp"

#include <utility>

namespace borophene::execution {

Result<void> RunPipeline(ChunkSource& source, ChunkSink& sink) {
  auto begin_result = sink.Begin(source.GetSchema());
  if (!begin_result) {
    return MakeUnexpected(std::move(begin_result.error()));
  }

  while (true) {
    auto next_result = source.Next();
    if (!next_result) {
      return MakeUnexpected(std::move(next_result.error()));
    }
    if (!next_result->has_value()) {
      break;
    }

    auto write_result = sink.Write(next_result->value());
    if (!write_result) {
      return MakeUnexpected(std::move(write_result.error()));
    }
  }

  return sink.Finish();
}

}  // namespace borophene::execution
