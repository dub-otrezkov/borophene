#pragma once

#include <span>
#include <string_view>

#include "borophene/common/result.hpp"
#include "borophene/io/csv_reader.hpp"

namespace borophene::io {

class CsvWriter {
 public:
  explicit CsvWriter(OutputStream& output, CsvOptions options = {});

  CsvWriter(const CsvWriter&) = delete;
  CsvWriter& operator=(const CsvWriter&) = delete;
  CsvWriter(CsvWriter&&) = delete;
  CsvWriter& operator=(CsvWriter&&) = delete;

  Result<void> WriteRow(std::span<const std::string_view> fields);

 private:
  OutputStream& output_;
  CsvOptions options_;
};

}  // namespace borophene::io
