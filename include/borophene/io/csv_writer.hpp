#ifndef BOROPHENE_IO_CSV_WRITER_HPP_
#define BOROPHENE_IO_CSV_WRITER_HPP_

#include <ostream>
#include <span>
#include <string_view>

#include "borophene/common/result.hpp"
#include "borophene/io/csv_reader.hpp"

namespace borophene::io {

class CsvWriter {
 public:
  explicit CsvWriter(std::ostream& output, CsvOptions options = {});

  CsvWriter(const CsvWriter&) = delete;
  CsvWriter& operator=(const CsvWriter&) = delete;
  CsvWriter(CsvWriter&&) = delete;
  CsvWriter& operator=(CsvWriter&&) = delete;

  [[nodiscard]] Result<void> WriteRow(std::span<const std::string_view> fields);

 private:
  std::ostream& output_;
  CsvOptions options_;
};

}  // namespace borophene::io

#endif  // BOROPHENE_IO_CSV_WRITER_HPP_
