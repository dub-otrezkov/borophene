#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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
static_assert(!std::is_copy_constructible_v<borophene::io::InputStream>);
static_assert(!std::is_copy_constructible_v<borophene::io::OutputStream>);
static_assert(!std::is_copy_constructible_v<borophene::io::MemoryStream>);
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

std::span<const borophene::Byte> AsBytes(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

std::string AsString(const borophene::io::MemoryStream& stream) {
  const auto data = stream.Data();
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

borophene::Result<Rows> ReadAll(borophene::io::InputStream& input, borophene::io::CsvOptions options = {}) {
  borophene::io::CsvReader reader(input, options);
  Rows rows;
  while (true) {
    auto row = reader.Next();
    if (!row) {
      return borophene::MakeUnexpected(std::move(row.error()));
    }
    if (!*row) {
      return rows;
    }
    rows.push_back(std::move(**row));
  }
}

borophene::Result<Rows> ReadAll(std::string_view text, borophene::io::CsvOptions options = {}) {
  borophene::io::MemoryStream input(AsBytes(text));
  return ReadAll(input, options);
}

void TestCsvRecordBoundaries() {
  using borophene::io::CsvReader;

  borophene::io::MemoryStream empty;
  CsvReader empty_reader(empty);
  auto no_row = empty_reader.Next();
  Check(no_row && !*no_row && empty_reader.RecordNumber() == 0, "empty input has no phantom record");

  Check(ReadAll("a,b") == borophene::Result<Rows>(Rows{{"a", "b"}}), "final record works without newline");
  Check(ReadAll("a,b\n") == borophene::Result<Rows>(Rows{{"a", "b"}}), "final record works with newline");
  Check(ReadAll("a,b\r\nc,d\r\n") == borophene::Result<Rows>(Rows{{"a", "b"}, {"c", "d"}}),
        "CRLF record endings are consumed");
  Check(ReadAll("a\rb") == borophene::Result<Rows>(Rows{{"a"}, {"b"}}),
        "a byte after a bare CR starts the next record");
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

class FailingInput final : public borophene::io::InputStream {
 public:
  explicit FailingInput(std::string prefix) : prefix_(std::move(prefix)) {
  }

  borophene::Result<std::size_t> Read(std::span<borophene::Byte> destination) override {
    if (destination.empty()) {
      return std::size_t{0};
    }
    if (position_ >= prefix_.size()) {
      return borophene::Failure<std::size_t>(borophene::ErrorCode::kIo, "simulated input failure");
    }
    destination.front() = static_cast<borophene::Byte>(static_cast<unsigned char>(prefix_[position_]));
    ++position_;
    return std::size_t{1};
  }

 private:
  std::string prefix_;
  std::size_t position_ = 0;
};

class CountingInput final : public borophene::io::InputStream {
 public:
  explicit CountingInput(std::string_view text,
                         std::size_t maximum_chunk_size = std::numeric_limits<std::size_t>::max())
      : input_(AsBytes(text)), maximum_chunk_size_(maximum_chunk_size) {
  }

  borophene::Result<std::size_t> Read(std::span<borophene::Byte> destination) override {
    ++read_calls_;
    const std::size_t count = std::min(destination.size(), maximum_chunk_size_);
    return input_.Read(destination.first(count));
  }

  std::size_t ReadCalls() const noexcept {
    return read_calls_;
  }

 private:
  borophene::io::MemoryStream input_;
  std::size_t maximum_chunk_size_;
  std::size_t read_calls_ = 0;
};

class FailingOutput final : public borophene::io::OutputStream {
 public:
  borophene::ui64 Position() const noexcept override {
    return 0;
  }

  borophene::Result<void> Write(std::span<const borophene::Byte>) override {
    return borophene::Failure<void>(borophene::ErrorCode::kIo, "simulated output failure");
  }

  borophene::Result<void> Flush() override {
    return {};
  }
};

void TestCsvReaderStreamFailures() {
  for (std::string prefix : {std::string{}, std::string{"\r"}}) {
    FailingInput input(std::move(prefix));
    borophene::io::CsvReader reader(input);

    auto result = reader.Next();
    Check(!result && result.error().Code() == borophene::ErrorCode::kIo, "reader propagates byte-stream I/O errors");
    Check(reader.RecordNumber() == 0, "failed input does not advance the CSV record number");
  }
}

void TestCsvReaderEof() {
  for (const auto& [text, expected] :
       std::array<std::pair<std::string_view, Rows>, 3>{{{"", {}}, {"a,b", {{"a", "b"}}}, {"value\r", {{"value"}}}}}) {
    Check(ReadAll(text) == borophene::Result<Rows>(expected), "byte streams retain the final CSV record");
  }
}

void TestCsvReaderBuffering() {
  constexpr std::size_t kRecordCount = 20'000;
  std::string text;
  text.reserve(kRecordCount * 4);
  for (std::size_t index = 0; index < kRecordCount; ++index) {
    text += "a,b\n";
  }

  CountingInput input(text);
  auto rows = ReadAll(input);
  Check(rows && rows->size() == kRecordCount, "buffered CSV input preserves every record");
  Check(input.ReadCalls() == 3, "buffered CSV input amortizes virtual backend reads");

  CountingInput chunked("\"a,b\"\r\nc,d", 2);
  Check(ReadAll(chunked) == borophene::Result<Rows>(Rows{{"a,b"}, {"c", "d"}}),
        "buffered CSV input handles token and CRLF boundaries split across backend reads");
}

void TestCsvWriter() {
  const std::vector<std::string> values{"plain", "a,b", "say \"hi\"", "line\nbreak", ""};
  std::vector<std::string_view> views(values.begin(), values.end());
  borophene::io::MemoryStream output;
  borophene::io::CsvWriter writer(output);
  auto written = writer.WriteRow(views);
  Check(written.has_value(), "CSV writer accepts a complete row");
  Check(AsString(output) == "plain,\"a,b\",\"say \"\"hi\"\"\",\"line\nbreak\",\n",
        "CSV writer quotes and escapes only fields that require it");
  output.Rewind();
  borophene::io::CsvReader reader(output);
  auto row = reader.Next();
  Check(row && *row && **row == values, "writer output round-trips through the same memory stream");

  FailingOutput failed_output;
  borophene::io::CsvWriter failed_writer(failed_output);
  auto failure = failed_writer.WriteRow(views);
  Check(!failure && failure.error().Code() == borophene::ErrorCode::kIo, "writer reports output failures");

  borophene::io::MemoryStream zero_field_output;
  borophene::io::CsvWriter zero_field_writer(zero_field_output);
  auto zero_fields = zero_field_writer.WriteRow(std::span<const std::string_view>{});
  Check(!zero_fields && zero_fields.error().Code() == borophene::ErrorCode::kInvalidArgument &&
            zero_field_output.Data().empty(),
        "writer rejects rows with no fields without producing an ambiguous blank record");

  const std::array<std::string_view, 1> one_empty_field{""};
  Check(zero_field_writer.WriteRow(one_empty_field).has_value() && AsString(zero_field_output) == "\n",
        "writer represents one empty field as a blank CSV record");
}

void TestCsvWriterLimits() {
  borophene::io::CsvOptions byte_limit;
  byte_limit.max_fields = 1;
  byte_limit.max_record_bytes = 6;

  const std::array<std::string_view, 1> exact_record{"a\"b"};
  borophene::io::MemoryStream exact_output;
  borophene::io::CsvWriter exact_writer(exact_output, byte_limit);
  Check(exact_writer.WriteRow(exact_record).has_value() && AsString(exact_output) == "\"a\"\"b\"\n",
        "writer accepts an exact encoded-byte boundary including escaped quotes");
  Check(ReadAll(AsString(exact_output), byte_limit) == borophene::Result<Rows>(Rows{{"a\"b"}}),
        "reader accepts writer output at the same encoded-byte boundary");

  const std::array<std::string_view, 1> oversized_record{"a\"bc"};
  borophene::io::MemoryStream oversized_output;
  borophene::io::CsvWriter oversized_writer(oversized_output, byte_limit);
  auto oversized = oversized_writer.WriteRow(oversized_record);
  Check(!oversized && oversized.error().Code() == borophene::ErrorCode::kInvalidArgument &&
            oversized_output.Data().empty(),
        "writer rejects an encoded record above the byte limit before writing");

  borophene::io::CsvOptions field_limit;
  field_limit.max_fields = 2;
  const std::array<std::string_view, 2> exact_fields{"a", "b"};
  borophene::io::MemoryStream exact_fields_output;
  borophene::io::CsvWriter exact_fields_writer(exact_fields_output, field_limit);
  Check(exact_fields_writer.WriteRow(exact_fields).has_value() &&
            ReadAll(AsString(exact_fields_output), field_limit) == borophene::Result<Rows>(Rows{{"a", "b"}}),
        "reader and writer accept the same exact field-count boundary");

  const std::array<std::string_view, 3> too_many_fields{"a", "b", "c"};
  borophene::io::MemoryStream too_many_fields_output;
  borophene::io::CsvWriter too_many_fields_writer(too_many_fields_output, field_limit);
  auto field_failure = too_many_fields_writer.WriteRow(too_many_fields);
  Check(!field_failure && field_failure.error().Code() == borophene::ErrorCode::kInvalidArgument &&
            too_many_fields_output.Data().empty(),
        "writer rejects records above the field limit before writing");
}

class ChunkedFile final : public borophene::io::RandomAccessFile {
 public:
  explicit ChunkedFile(std::string data) : data_(std::move(data)) {
  }

  borophene::Result<borophene::ui64> Size() const override {
    return data_.size();
  }

  borophene::Result<std::size_t> ReadAt(borophene::ui64 offset, std::span<borophene::Byte> destination) const override {
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

void TestMemoryStream() {
  using namespace borophene;
  using namespace borophene::io;

  MemoryStream output;
  auto shared_input = output.Share();
  Check(output.Write(AsBytes("abc")).has_value() && output.Position() == 3,
        "memory stream appends bytes and tracks its position");
  Check(shared_input->Size() == Result<ui64>(3), "shared memory stream views observe appended bytes");

  std::array<Byte, 2> prefix{};
  Check(shared_input->Read(prefix) == Result<std::size_t>(2) && std::memcmp(prefix.data(), "ab", prefix.size()) == 0,
        "memory stream supports sequential input");
  std::array<Byte, 3> all{};
  Check(ReadExactly(*shared_input, 0, all).has_value() && std::memcmp(all.data(), "abc", all.size()) == 0,
        "memory stream supports random-access input");

  shared_input->Rewind();
  Check(shared_input->Read(all) == Result<std::size_t>(3), "memory stream sequential input can be rewound");
  Check(output.Flush().has_value() && shared_input->IsFlushed(), "shared memory stream views observe flushes");
  Check(output.Write(AsBytes("d")).has_value() && !shared_input->IsFlushed(),
        "writing marks shared memory stream data as unflushed");

  const auto complete_data = output.Data();
  Check(output.Write(complete_data).has_value() && AsString(output) == "abcdabcd",
        "memory stream safely appends its complete current contents");
  const auto shared_subspan = shared_input->Data().subspan(1, 2);
  Check(output.Write(shared_subspan).has_value() && AsString(output) == "abcdabcdbc",
        "memory stream safely appends a subspan from a shared view");
}

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
  auto overflow = ReadExactly(chunked, std::numeric_limits<ui64>::max(), overflow_byte);
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
  Check((*input)->Size() == Result<ui64>(payload.size()), "local input reports file size");
  std::vector<Byte> bytes(payload.size());
  Check(ReadExactly(**input, 0, bytes).has_value(), "local input supports exact reads");
  Check(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0, "local file data round-trips");

  auto sequential_input = OpenLocalInputStream(temporary.Get());
  Check(sequential_input.has_value(), "local sequential input stream is opened");
  if (!sequential_input) {
    return;
  }
  std::vector<Byte> sequential_bytes(payload.size());
  std::size_t total = 0;
  while (total < sequential_bytes.size()) {
    auto read = (*sequential_input)->Read(std::span(sequential_bytes).subspan(total));
    Check(read.has_value(), "local sequential input reads bytes");
    if (!read || *read == 0) {
      break;
    }
    total += *read;
  }
  Check(total == payload.size() && std::memcmp(sequential_bytes.data(), payload.data(), payload.size()) == 0,
        "local sequential input round-trips file data");

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
    TestCsvReaderEof();
    TestCsvReaderBuffering();
    TestCsvWriter();
    TestCsvWriterLimits();
    TestMemoryStream();
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
