#include "borophene/io/csv_reader.hpp"

#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace borophene::io {
namespace {

enum class CsvState : std::uint8_t { kFieldStart, kUnquoted, kQuoted, kAfterQuote };

[[nodiscard]] Result<void> ValidateOptions(const CsvOptions& options) {
  if (options.delimiter == options.quote) {
    return Failure<void>(ErrorCode::kInvalidArgument, "CSV delimiter and quote must be different");
  }
  if (options.delimiter == '\r' || options.delimiter == '\n') {
    return Failure<void>(ErrorCode::kInvalidArgument, "CSV delimiter cannot be a line-ending character");
  }
  if (options.quote == '\r' || options.quote == '\n') {
    return Failure<void>(ErrorCode::kInvalidArgument, "CSV quote cannot be a line-ending character");
  }
  if (options.max_fields == 0) {
    return Failure<void>(ErrorCode::kInvalidArgument, "CSV field limit must be greater than zero");
  }
  return {};
}

[[nodiscard]] std::string Context(Index record, std::size_t field) {
  return "CSV record " + std::to_string(record) + ", field " + std::to_string(field) + ": ";
}

}  // namespace

CsvReader::CsvReader(std::istream& input, CsvOptions options) : input_(input), options_(options) {}

Result<std::optional<std::vector<std::string>>> CsvReader::Next() {
  using NextResult = Result<std::optional<std::vector<std::string>>>;

  auto validation = ValidateOptions(options_);
  if (!validation) return std::unexpected(validation.error());

  const Index current_record = record_number_ + 1;
  std::vector<std::string> fields;
  std::string field;
  CsvState state = CsvState::kFieldStart;
  std::size_t record_bytes = 0;
  bool saw_input = false;

  const auto malformed = [&](std::string message) -> NextResult {
    return Failure<std::optional<std::vector<std::string>>>(
        ErrorCode::kMalformedCsv, Context(current_record, fields.size() + 1) + std::move(message));
  };
  const auto stream_failure = [&]() -> NextResult {
    return Failure<std::optional<std::vector<std::string>>>(
        ErrorCode::kIo, Context(current_record, fields.size() + 1) + "input stream failure");
  };
  const auto account_byte = [&]() -> Result<void> {
    if (record_bytes >= options_.max_record_bytes) {
      return Failure<void>(ErrorCode::kMalformedCsv, Context(current_record, fields.size() + 1) +
                                                         "record exceeds the byte limit of " +
                                                         std::to_string(options_.max_record_bytes));
    }
    ++record_bytes;
    return {};
  };
  const auto append_field = [&]() -> Result<void> {
    if (fields.size() >= options_.max_fields) {
      return Failure<void>(ErrorCode::kMalformedCsv, Context(current_record, fields.size() + 1) +
                                                         "record exceeds the field limit of " +
                                                         std::to_string(options_.max_fields));
    }
    fields.push_back(std::move(field));
    field.clear();
    return {};
  };
  const auto finish_record = [&]() -> NextResult {
    auto appended = append_field();
    if (!appended) return std::unexpected(appended.error());
    ++record_number_;
    return std::optional<std::vector<std::string>>(std::move(fields));
  };
  const auto consume_lf = [&]() -> Result<void> {
    try {
      const int next = input_.peek();
      if (next == std::char_traits<char>::eof()) {
        if (input_.bad() || (!input_.eof() && input_.fail())) {
          return Failure<void>(ErrorCode::kIo, Context(current_record, fields.size() + 1) + "input stream failure");
        }
        return {};
      }
      if (std::char_traits<char>::to_char_type(next) == '\n') {
        static_cast<void>(input_.get());
        if (!input_) {
          return Failure<void>(ErrorCode::kIo, Context(current_record, fields.size() + 1) + "input stream failure");
        }
      }
    } catch (const std::ios_base::failure& failure) {
      if (input_.eof() && !input_.bad()) {
        return {};
      }
      return Failure<void>(ErrorCode::kIo,
                           Context(current_record, fields.size() + 1) + "input stream failure: " + failure.what());
    }
    return {};
  };

  try {
    while (true) {
      const int value = input_.get();
      if (value == std::char_traits<char>::eof()) {
        if (input_.bad() || (!input_.eof() && input_.fail())) return stream_failure();
        if (state == CsvState::kQuoted) return malformed("unclosed quoted field");
        if (!saw_input) return std::optional<std::vector<std::string>>{};
        return finish_record();
      }

      saw_input = true;
      const char character = std::char_traits<char>::to_char_type(value);
      switch (state) {
        case CsvState::kFieldStart:
          if (character == options_.delimiter) {
            auto accounted = account_byte();
            if (!accounted) return std::unexpected(accounted.error());
            auto appended = append_field();
            if (!appended) return std::unexpected(appended.error());
          } else if (character == options_.quote) {
            auto accounted = account_byte();
            if (!accounted) return std::unexpected(accounted.error());
            state = CsvState::kQuoted;
          } else if (character == '\n') {
            return finish_record();
          } else if (character == '\r') {
            auto consumed = consume_lf();
            if (!consumed) return std::unexpected(consumed.error());
            return finish_record();
          } else {
            auto accounted = account_byte();
            if (!accounted) return std::unexpected(accounted.error());
            field.push_back(character);
            state = CsvState::kUnquoted;
          }
          break;

        case CsvState::kUnquoted:
          if (character == options_.delimiter) {
            auto accounted = account_byte();
            if (!accounted) return std::unexpected(accounted.error());
            auto appended = append_field();
            if (!appended) return std::unexpected(appended.error());
            state = CsvState::kFieldStart;
          } else if (character == options_.quote) {
            return malformed("quote inside an unquoted field");
          } else if (character == '\n') {
            return finish_record();
          } else if (character == '\r') {
            auto consumed = consume_lf();
            if (!consumed) return std::unexpected(consumed.error());
            return finish_record();
          } else {
            auto accounted = account_byte();
            if (!accounted) return std::unexpected(accounted.error());
            field.push_back(character);
          }
          break;

        case CsvState::kQuoted: {
          auto accounted = account_byte();
          if (!accounted) return std::unexpected(accounted.error());
          if (character == options_.quote) {
            state = CsvState::kAfterQuote;
          } else {
            field.push_back(character);
          }
          break;
        }

        case CsvState::kAfterQuote:
          if (character == options_.quote) {
            auto accounted = account_byte();
            if (!accounted) return std::unexpected(accounted.error());
            field.push_back(character);
            state = CsvState::kQuoted;
          } else if (character == options_.delimiter) {
            auto accounted = account_byte();
            if (!accounted) return std::unexpected(accounted.error());
            auto appended = append_field();
            if (!appended) return std::unexpected(appended.error());
            state = CsvState::kFieldStart;
          } else if (character == '\n') {
            return finish_record();
          } else if (character == '\r') {
            auto consumed = consume_lf();
            if (!consumed) return std::unexpected(consumed.error());
            return finish_record();
          } else {
            return malformed("unexpected character after a closing quote");
          }
          break;
      }
    }
  } catch (const std::ios_base::failure& failure) {
    if (input_.eof() && !input_.bad()) {
      if (state == CsvState::kQuoted) return malformed("unclosed quoted field");
      if (!saw_input) return std::optional<std::vector<std::string>>{};
      return finish_record();
    }
    return Failure<std::optional<std::vector<std::string>>>(
        ErrorCode::kIo, Context(current_record, fields.size() + 1) + "input stream failure: " + failure.what());
  }
}

}  // namespace borophene::io
