#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "borophene/common/error.hpp"
#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"
#include "borophene/io/csv_reader.hpp"
#include "borophene/io/csv_writer.hpp"
#include "borophene/io/file.hpp"

static_assert(!std::is_copy_constructible_v<borophene::io::RandomAccessFile>);
static_assert(!std::is_copy_constructible_v<borophene::io::OutputFile>);
static_assert(!std::is_copy_constructible_v<borophene::io::CsvReader>);
static_assert(!std::is_move_constructible_v<borophene::io::CsvReader>);
static_assert(!std::is_copy_assignable_v<borophene::io::CsvReader>);
static_assert(!std::is_move_assignable_v<borophene::io::CsvReader>);
static_assert(!std::is_copy_constructible_v<borophene::io::CsvWriter>);
static_assert(!std::is_move_constructible_v<borophene::io::CsvWriter>);
static_assert(!std::is_copy_assignable_v<borophene::io::CsvWriter>);
static_assert(!std::is_move_assignable_v<borophene::io::CsvWriter>);

namespace {

int failures = 0;

void Check(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

using Rows = std::vector<std::vector<std::string>>;

borophene::Result<Rows> ReadAll(std::string_view text, borophene::io::CsvOptions options = {}) {
  std::istringstream input{std::string(text)};
  borophene::io::CsvReader reader(input, options);
  Rows rows;
  while (true) {
    auto row = reader.Next();
    if (!row) {
      return std::unexpected(row.error());
    }
    if (!*row) {
      return rows;
    }
    rows.push_back(std::move(**row));
  }
}

void TestCsvRecordBoundaries() {
  using borophene::io::CsvReader;

  std::istringstream empty;
  CsvReader empty_reader(empty);
  auto no_row = empty_reader.Next();
  Check(no_row && !*no_row && empty_reader.RecordNumber() == 0, "empty input has no phantom record");

  Check(ReadAll("a,b") == borophene::Result<Rows>(Rows{{"a", "b"}}), "final record works without newline");
  Check(ReadAll("a,b\n") == borophene::Result<Rows>(Rows{{"a", "b"}}), "final record works with newline");
  Check(ReadAll("a,b\r\nc,d\r\n") == borophene::Result<Rows>(Rows{{"a", "b"}, {"c", "d"}}),
        "CRLF record endings are consumed");
  Check(ReadAll("\n,,\nvalue,\n") == borophene::Result<Rows>(Rows{{""}, {"", "", ""}, {"value", ""}}),
        "blank records and trailing empty fields are preserved");
  Check(ReadAll(" a ,\tb \n") == borophene::Result<Rows>(Rows{{" a ", "\tb "}}), "unquoted whitespace is preserved");

  borophene::io::CsvOptions semicolon;
  semicolon.delimiter = ';';
  Check(ReadAll("one;two,three\n", semicolon) == borophene::Result<Rows>(Rows{{"one", "two,three"}}),
        "custom delimiters are honored");
}

void TestQuotedCsv() {
  const auto parsed = ReadAll("\"a,b\",\"say \"\"hi\"\"\",\"line1\nline2\"\n");
  Check(parsed == borophene::Result<Rows>(Rows{{"a,b", "say \"hi\"", "line1\nline2"}}),
        "quoted delimiters, doubled quotes, and newlines are decoded");

  for (const std::string_view malformed : {"abc\"def\n", "\"abc\"junk\n", "\"abc"}) {
    auto result = ReadAll(malformed);
    Check(!result && result.error().Code() == borophene::ErrorCode::kMalformedCsv, "malformed quoting is rejected");
    if (!result) {
      Check(result.error().Message().find("record 1, field 1") != std::string::npos,
            "malformed CSV errors identify record and field");
    }
  }
}

void TestCsvLimits() {
  borophene::io::CsvOptions byte_limit;
  byte_limit.max_record_bytes = 3;
  Check(ReadAll("a,b\n", byte_limit).has_value(), "record byte limit accepts an exact fit");
  auto oversized = ReadAll("abcd\n", byte_limit);
  Check(!oversized && oversized.error().Code() == borophene::ErrorCode::kMalformedCsv &&
            oversized.error().Message().find("record 1, field 1") != std::string::npos,
        "record byte limit reports CSV context");

  borophene::io::CsvOptions field_limit;
  field_limit.max_fields = 2;
  auto too_many_fields = ReadAll("a,b,c\n", field_limit);
  Check(!too_many_fields && too_many_fields.error().Code() == borophene::ErrorCode::kMalformedCsv &&
            too_many_fields.error().Message().find("field 3") != std::string::npos,
        "field limit reports the offending field");
}

class FailingStreamBuffer final : public std::streambuf {
 protected:
  int_type overflow(int_type) override {
    return traits_type::eof();
  }

  std::streamsize xsputn(const char*, std::streamsize) override {
    return 0;
  }
};

class ThrowingInputBuffer final : public std::streambuf {
 public:
  explicit ThrowingInputBuffer(std::string prefix) : prefix_(std::move(prefix)) {
  }

 protected:
  int_type underflow() override {
    if (position_ < prefix_.size()) {
      return traits_type::to_int_type(prefix_[position_]);
    }
    throw std::ios_base::failure("simulated input failure");
  }

  int_type uflow() override {
    const int_type value = underflow();
    ++position_;
    return value;
  }

 private:
  std::string prefix_;
  std::size_t position_ = 0;
};

void TestCsvReaderStreamFailures() {
  for (std::string prefix : {std::string{}, std::string{"\r"}}) {
    ThrowingInputBuffer buffer(std::move(prefix));
    std::istream input(&buffer);
    input.exceptions(std::ios::badbit);
    borophene::io::CsvReader reader(input);

    try {
      auto result = reader.Next();
      Check(!result && result.error().Code() == borophene::ErrorCode::kIo,
            "reader translates exception-enabled stream failures to I/O errors");
      Check(reader.RecordNumber() == 0, "failed input does not advance the CSV record number");
    } catch (...) {
      Check(false, "reader must not leak stream I/O exceptions");
    }
  }
}

void TestCsvReaderExceptionEnabledEof() {
  for (const auto& [text, expected] :
       std::array<std::pair<std::string_view, Rows>, 3>{{{"", {}}, {"a,b", {{"a", "b"}}}, {"value\r", {{"value"}}}}}) {
    std::istringstream input{std::string(text)};
    input.exceptions(std::ios::eofbit | std::ios::failbit | std::ios::badbit);
    borophene::io::CsvReader reader(input);
    Rows actual;
    try {
      while (true) {
        auto row = reader.Next();
        Check(row.has_value(), "exception-enabled streams preserve clean EOF");
        if (!row || !*row) {
          break;
        }
        actual.push_back(std::move(**row));
      }
    } catch (...) {
      Check(false, "reader must not leak clean EOF exceptions");
    }
    Check(actual == expected, "exception-enabled streams retain the final CSV record");
  }
}

void TestCsvWriter() {
  const std::vector<std::string> values{"plain", "a,b", "say \"hi\"", "line\nbreak", ""};
  std::vector<std::string_view> views(values.begin(), values.end());
  std::ostringstream output;
  borophene::io::CsvWriter writer(output);
  auto written = writer.WriteRow(views);
  Check(written.has_value(), "CSV writer accepts a complete row");
  Check(output.str() == "plain,\"a,b\",\"say \"\"hi\"\"\",\"line\nbreak\",\n",
        "CSV writer quotes and escapes only fields that require it");
  Check(ReadAll(output.str()) == borophene::Result<Rows>(Rows{values}), "writer output round-trips through reader");

  FailingStreamBuffer buffer;
  std::ostream failed_output(&buffer);
  borophene::io::CsvWriter failed_writer(failed_output);
  auto failure = failed_writer.WriteRow(views);
  Check(!failure && failure.error().Code() == borophene::ErrorCode::kIo, "writer reports output failures");

  std::ostringstream zero_field_output;
  borophene::io::CsvWriter zero_field_writer(zero_field_output);
  auto zero_fields = zero_field_writer.WriteRow(std::span<const std::string_view>{});
  Check(!zero_fields && zero_fields.error().Code() == borophene::ErrorCode::kInvalidArgument &&
            zero_field_output.str().empty(),
        "writer rejects rows with no fields without producing an ambiguous blank record");

  const std::array<std::string_view, 1> one_empty_field{""};
  Check(zero_field_writer.WriteRow(one_empty_field).has_value() && zero_field_output.str() == "\n",
        "writer represents one empty field as a blank CSV record");
}

void TestCsvWriterLimits() {
  borophene::io::CsvOptions byte_limit;
  byte_limit.max_fields = 1;
  byte_limit.max_record_bytes = 6;

  const std::array<std::string_view, 1> exact_record{"a\"b"};
  std::ostringstream exact_output;
  borophene::io::CsvWriter exact_writer(exact_output, byte_limit);
  Check(exact_writer.WriteRow(exact_record).has_value() && exact_output.str() == "\"a\"\"b\"\n",
        "writer accepts an exact encoded-byte boundary including escaped quotes");
  Check(ReadAll(exact_output.str(), byte_limit) == borophene::Result<Rows>(Rows{{"a\"b"}}),
        "reader accepts writer output at the same encoded-byte boundary");

  const std::array<std::string_view, 1> oversized_record{"a\"bc"};
  std::ostringstream oversized_output;
  borophene::io::CsvWriter oversized_writer(oversized_output, byte_limit);
  auto oversized = oversized_writer.WriteRow(oversized_record);
  Check(!oversized && oversized.error().Code() == borophene::ErrorCode::kInvalidArgument &&
            oversized_output.str().empty(),
        "writer rejects an encoded record above the byte limit before writing");

  borophene::io::CsvOptions field_limit;
  field_limit.max_fields = 2;
  const std::array<std::string_view, 2> exact_fields{"a", "b"};
  std::ostringstream exact_fields_output;
  borophene::io::CsvWriter exact_fields_writer(exact_fields_output, field_limit);
  Check(exact_fields_writer.WriteRow(exact_fields).has_value() &&
            ReadAll(exact_fields_output.str(), field_limit) == borophene::Result<Rows>(Rows{{"a", "b"}}),
        "reader and writer accept the same exact field-count boundary");

  const std::array<std::string_view, 3> too_many_fields{"a", "b", "c"};
  std::ostringstream too_many_fields_output;
  borophene::io::CsvWriter too_many_fields_writer(too_many_fields_output, field_limit);
  auto field_failure = too_many_fields_writer.WriteRow(too_many_fields);
  Check(!field_failure && field_failure.error().Code() == borophene::ErrorCode::kInvalidArgument &&
            too_many_fields_output.str().empty(),
        "writer rejects records above the field limit before writing");
}

class ChunkedFile final : public borophene::io::RandomAccessFile {
 public:
  explicit ChunkedFile(std::string data) : data_(std::move(data)) {
  }

  borophene::Result<std::uint64_t> Size() const override {
    return data_.size();
  }

  borophene::Result<std::size_t> ReadAt(std::uint64_t offset, std::span<borophene::Byte> destination) const override {
    if (offset >= data_.size()) {
      return std::size_t{0};
    }
    const std::size_t available = data_.size() - static_cast<std::size_t>(offset);
    const std::size_t count = std::min({available, destination.size(), std::size_t{2}});
    std::memcpy(destination.data(), data_.data() + static_cast<std::size_t>(offset), count);
    return count;
  }

 private:
  std::string data_;
};

class TemporaryPath {
 public:
  TemporaryPath()
      : path_(std::filesystem::temp_directory_path() /
              ("borophene-csv-io-test-" + std::to_string(static_cast<long long>(::getpid())))) {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  ~TemporaryPath() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::filesystem::path& Get() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

void TestFiles() {
  using namespace borophene;
  using namespace borophene::io;

  ChunkedFile chunked("abcdef");
  std::array<Byte, 6> chunked_bytes{};
  Check(ReadExactly(chunked, 0, chunked_bytes).has_value(), "ReadExactly handles partial reads");
  Check(std::memcmp(chunked_bytes.data(), "abcdef", chunked_bytes.size()) == 0,
        "ReadExactly advances offsets and destinations");
  std::array<Byte, 7> short_bytes{};
  auto short_read = ReadExactly(chunked, 0, short_bytes);
  Check(!short_read && short_read.error().Code() == ErrorCode::kIo, "ReadExactly detects premature EOF");
  std::array<Byte, 1> overflow_byte{};
  auto overflow = ReadExactly(chunked, std::numeric_limits<std::uint64_t>::max(), overflow_byte);
  Check(!overflow && overflow.error().Code() == ErrorCode::kOutOfRange, "ReadExactly detects offset overflow");

  TemporaryPath temporary;
  const std::string payload = "local-file-payload";
  auto output = CreateLocalOutput(temporary.Get());
  Check(output.has_value(), "local output file is created");
  if (!output) {
    return;
  }
  auto payload_bytes = std::as_bytes(std::span(payload.data(), payload.size()));
  Check((*output)->Write(payload_bytes).has_value() && (*output)->Position() == payload.size(),
        "local output writes all bytes and tracks position");
  Check((*output)->Flush().has_value(), "local output flushes to storage");
  output->reset();

  auto input = OpenLocalInput(temporary.Get());
  Check(input.has_value(), "local input file is opened");
  if (!input) {
    return;
  }
  Check((*input)->Size() == Result<std::uint64_t>(payload.size()), "local input reports file size");
  std::vector<Byte> bytes(payload.size());
  Check(ReadExactly(**input, 0, bytes).has_value(), "local input supports exact reads");
  Check(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0, "local file data round-trips");

  auto missing = OpenLocalInput(temporary.Get().string() + ".missing");
  Check(!missing && missing.error().Code() == ErrorCode::kIo &&
            missing.error().Message().find(".missing") != std::string::npos,
        "local file errors retain the path");
}

}  // namespace

int main() {
  try {
    TestCsvRecordBoundaries();
    TestQuotedCsv();
    TestCsvLimits();
    TestCsvReaderStreamFailures();
    TestCsvReaderExceptionEnabledEof();
    TestCsvWriter();
    TestCsvWriterLimits();
    TestFiles();
  } catch (const std::exception& exception) {
    std::cerr << "unexpected exception: " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "unexpected non-standard exception\n";
    return 1;
  }
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
  }
  return failures == 0 ? 0 : 1;
}
