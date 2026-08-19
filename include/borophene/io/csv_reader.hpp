#ifndef BOROPHENE_IO_CSV_READER_HPP_
#define BOROPHENE_IO_CSV_READER_HPP_

#include <cstddef>
#include <istream>
#include <optional>
#include <string>
#include <vector>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"

namespace borophene::io {

struct CsvOptions {
  char delimiter = ',';
  char quote = '"';
  std::size_t max_record_bytes = std::size_t{16} * 1024U * 1024U;
  std::size_t max_fields = 1024U;
};

class CsvReader {
 public:
  explicit CsvReader(std::istream& input, CsvOptions options = {});

  CsvReader(const CsvReader&) = delete;
  CsvReader& operator=(const CsvReader&) = delete;
  CsvReader(CsvReader&&) = delete;
  CsvReader& operator=(CsvReader&&) = delete;

  [[nodiscard]] Result<std::optional<std::vector<std::string>>> Next();
  [[nodiscard]] Index RecordNumber() const noexcept { return record_number_; }

 private:
  std::istream& input_;
  CsvOptions options_;
  Index record_number_ = 0;
};

}  // namespace borophene::io

#endif  // BOROPHENE_IO_CSV_READER_HPP_
