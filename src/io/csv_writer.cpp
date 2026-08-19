#include "borophene/io/csv_writer.hpp"

#include <array>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace borophene::io {
namespace {

// A quote inside a quoted field is doubled, adding one encoded byte.
constexpr std::size_t kEscapedQuoteExtraBytes = 1;
// A quoted field adds one opening and one closing quote.
constexpr std::size_t kSurroundingQuoteBytes = 2;
// Every field after the first contributes one delimiter.
constexpr std::size_t kDelimiterBytes = 1;

Result<void> ValidateOptions(const CsvOptions& options) {
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

bool NeedsQuotes(std::string_view field, const CsvOptions& options) noexcept {
  return field.find(options.delimiter) != std::string_view::npos ||
         field.find(options.quote) != std::string_view::npos || field.find('\r') != std::string_view::npos ||
         field.find('\n') != std::string_view::npos;
}

bool CheckedAdd(std::size_t increment, std::size_t& total) noexcept {
  if (increment > std::numeric_limits<std::size_t>::max() - total) {
    return false;
  }
  total += increment;
  return true;
}

Result<std::size_t> EncodedFieldSize(std::string_view field, const CsvOptions& options) {
  if (!NeedsQuotes(field, options)) {
    return field.size();
  }

  std::size_t encoded_size = field.size();
  for (const char character : field) {
    if (character == options.quote && !CheckedAdd(kEscapedQuoteExtraBytes, encoded_size)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV field size exceeds the addressable range");
    }
  }
  if (!CheckedAdd(kSurroundingQuoteBytes, encoded_size)) {
    return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV field size exceeds the addressable range");
  }
  return encoded_size;
}

Result<std::size_t> EncodedRecordSize(std::span<const std::string_view> fields, const CsvOptions& options) {
  std::size_t encoded_size = 0;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0 && !CheckedAdd(kDelimiterBytes, encoded_size)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV record size exceeds the addressable range");
    }
    auto field_size = EncodedFieldSize(fields[index], options);
    if (!field_size) {
      return MakeUnexpected(std::move(field_size.error()));
    }
    if (!CheckedAdd(*field_size, encoded_size)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV record size exceeds the addressable range");
    }
  }
  return encoded_size;
}

Result<void> WriteCharacter(OutputStream& output, char character) {
  const std::array<Byte, 1> data{static_cast<Byte>(static_cast<unsigned char>(character))};
  return output.Write(data);
}

Result<void> WriteText(OutputStream& output, std::string_view text) {
  return output.Write(std::as_bytes(std::span(text.data(), text.size())));
}

}  // namespace

CsvWriter::CsvWriter(OutputStream& output, CsvOptions options) : output_(output), options_(options) {
}

Result<void> CsvWriter::WriteRow(std::span<const std::string_view> fields) {
  auto validation = ValidateOptions(options_);
  if (!validation) {
    return MakeUnexpected(std::move(validation.error()));
  }
  if (fields.empty()) {
    return Failure<void>(ErrorCode::kInvalidArgument, "CSV rows must contain at least one field");
  }
  if (fields.size() > options_.max_fields) {
    return Failure<void>(ErrorCode::kInvalidArgument,
                         "CSV row exceeds the field limit of " + std::to_string(options_.max_fields));
  }

  auto encoded_size = EncodedRecordSize(fields, options_);
  if (!encoded_size) {
    return MakeUnexpected(std::move(encoded_size.error()));
  }
  if (*encoded_size > options_.max_record_bytes) {
    return Failure<void>(ErrorCode::kInvalidArgument,
                         "encoded CSV row exceeds the byte limit of " + std::to_string(options_.max_record_bytes));
  }

  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) {
      auto delimiter = WriteCharacter(output_, options_.delimiter);
      if (!delimiter) {
        return delimiter;
      }
    }

    const std::string_view field = fields[index];
    if (!NeedsQuotes(field, options_)) {
      auto written = WriteText(output_, field);
      if (!written) {
        return written;
      }
      continue;
    }

    auto opening_quote = WriteCharacter(output_, options_.quote);
    if (!opening_quote) {
      return opening_quote;
    }
    for (const char character : field) {
      auto written = WriteCharacter(output_, character);
      if (!written) {
        return written;
      }
      if (character == options_.quote) {
        auto escaped = WriteCharacter(output_, options_.quote);
        if (!escaped) {
          return escaped;
        }
      }
    }
    auto closing_quote = WriteCharacter(output_, options_.quote);
    if (!closing_quote) {
      return closing_quote;
    }
  }

  return WriteCharacter(output_, '\n');
}

}  // namespace borophene::io
