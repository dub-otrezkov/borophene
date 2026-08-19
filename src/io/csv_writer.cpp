#include "borophene/io/csv_writer.hpp"

#include <algorithm>
#include <ios>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace borophene::io {
namespace {

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

Result<void> StreamFailure(std::string_view detail = {}) {
  std::string message = "cannot write CSV row";
  if (!detail.empty()) {
    message += ": ";
    message += detail;
  }
  return Failure<void>(ErrorCode::kIo, std::move(message));
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
    if (character == options.quote && !CheckedAdd(1, encoded_size)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV field size exceeds the addressable range");
    }
  }
  if (!CheckedAdd(2, encoded_size)) {
    return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV field size exceeds the addressable range");
  }
  return encoded_size;
}

Result<std::size_t> EncodedRecordSize(std::span<const std::string_view> fields, const CsvOptions& options) {
  std::size_t encoded_size = 0;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0 && !CheckedAdd(1, encoded_size)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV record size exceeds the addressable range");
    }
    auto field_size = EncodedFieldSize(fields[index], options);
    if (!field_size) {
      return std::unexpected(field_size.error());
    }
    if (!CheckedAdd(*field_size, encoded_size)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange, "encoded CSV record size exceeds the addressable range");
    }
  }
  return encoded_size;
}

Result<void> WriteCharacter(std::ostream& output, char character) {
  try {
    output.put(character);
  } catch (const std::ios_base::failure& failure) {
    return StreamFailure(failure.what());
  }
  if (!output) {
    return StreamFailure();
  }
  return {};
}

Result<void> WriteText(std::ostream& output, std::string_view text) {
  std::size_t offset = 0;
  constexpr auto kMaximumStreamSize = static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());
  try {
    while (offset < text.size()) {
      const std::size_t count = std::min(text.size() - offset, kMaximumStreamSize);
      output.write(text.data() + offset, static_cast<std::streamsize>(count));
      if (!output) {
        return StreamFailure();
      }
      offset += count;
    }
  } catch (const std::ios_base::failure& failure) {
    return StreamFailure(failure.what());
  }
  return {};
}

}  // namespace

CsvWriter::CsvWriter(std::ostream& output, CsvOptions options) : output_(output), options_(options) {
}

Result<void> CsvWriter::WriteRow(std::span<const std::string_view> fields) {
  auto validation = ValidateOptions(options_);
  if (!validation) {
    return std::unexpected(validation.error());
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
    return std::unexpected(encoded_size.error());
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
