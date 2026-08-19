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

enum class CsvState : std::uint8_t {
  kFieldStart,
  kUnquoted,
  kQuoted,
  kAfterQuote
};

enum class CsvParseAction : std::uint8_t {
  kContinue,
  kFinishRecord
};

using CsvRow = std::vector<std::string>;
using CsvReadResult = Result<std::optional<CsvRow>>;
using CsvStepResult = Result<CsvParseAction>;

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

std::string CsvContext(Index record, std::size_t field) {
  return "CSV record " + std::to_string(record) + ", field " + std::to_string(field) + ": ";
}

class CsvRecordParser {
 public:
  CsvRecordParser(std::istream& input, const CsvOptions& options, Index& record_number)
      : input_(input), options_(options), record_number_(record_number), current_record_(record_number + 1) {
  }

  CsvReadResult Parse() {
    try {
      while (true) {
        const int value = input_.get();
        if (value == std::char_traits<char>::eof()) {
          return HandleEndOfInput();
        }

        saw_input_ = true;
        const char character = std::char_traits<char>::to_char_type(value);
        auto step = HandleCharacter(character);
        if (!step) {
          return std::unexpected(step.error());
        }
        if (*step == CsvParseAction::kFinishRecord) {
          return FinishRecord();
        }
      }
    } catch (const std::ios_base::failure& failure) {
      return HandleInputFailure(failure);
    }
  }

 private:
  Error MalformedError(std::string message) const {
    return {ErrorCode::kMalformedCsv, CurrentContext() + std::move(message)};
  }

  CsvReadResult StreamFailure(const char* detail = nullptr) const {
    std::string message = CurrentContext() + "input stream failure";
    if (detail != nullptr) {
      message += ": ";
      message += detail;
    }
    return std::unexpected(Error(ErrorCode::kIo, std::move(message)));
  }

  std::string CurrentContext() const {
    return CsvContext(current_record_, fields_.size() + 1);
  }

  Result<void> AccountByte() {
    if (record_bytes_ >= options_.max_record_bytes) {
      return Failure<void>(ErrorCode::kMalformedCsv, CurrentContext() + "record exceeds the byte limit of " +
                                                         std::to_string(options_.max_record_bytes));
    }
    ++record_bytes_;
    return {};
  }

  Result<void> AppendField() {
    if (fields_.size() >= options_.max_fields) {
      return Failure<void>(ErrorCode::kMalformedCsv, CurrentContext() + "record exceeds the field limit of " +
                                                         std::to_string(options_.max_fields));
    }
    fields_.push_back(std::move(field_));
    field_.clear();
    return {};
  }

  CsvReadResult FinishRecord() {
    auto appended = AppendField();
    if (!appended) {
      return std::unexpected(appended.error());
    }
    ++record_number_;
    return std::optional<CsvRow>(std::move(fields_));
  }

  Result<void> ConsumeLineFeed() {
    try {
      const int next = input_.peek();
      if (next == std::char_traits<char>::eof()) {
        if (input_.bad() || (!input_.eof() && input_.fail())) {
          return Failure<void>(ErrorCode::kIo, CurrentContext() + "input stream failure");
        }
        return {};
      }
      if (std::char_traits<char>::to_char_type(next) == '\n') {
        static_cast<void>(input_.get());
        if (!input_) {
          return Failure<void>(ErrorCode::kIo, CurrentContext() + "input stream failure");
        }
      }
    } catch (const std::ios_base::failure& failure) {
      if (input_.eof() && !input_.bad()) {
        return {};
      }
      return Failure<void>(ErrorCode::kIo, CurrentContext() + "input stream failure: " + failure.what());
    }
    return {};
  }

  CsvStepResult HandleCharacter(char character) {
    switch (state_) {
      case CsvState::kFieldStart:
        return HandleFieldStart(character);
      case CsvState::kUnquoted:
        return HandleUnquoted(character);
      case CsvState::kQuoted:
        return HandleQuoted(character);
      case CsvState::kAfterQuote:
        return HandleAfterQuote(character);
    }
    return Failure<CsvParseAction>(ErrorCode::kInvalidState, "CSV parser entered an invalid state");
  }

  CsvStepResult HandleFieldStart(char character) {
    if (character == options_.delimiter) {
      auto accounted = AccountByte();
      if (!accounted) {
        return std::unexpected(accounted.error());
      }
      auto appended = AppendField();
      if (!appended) {
        return std::unexpected(appended.error());
      }
    } else if (character == options_.quote) {
      auto accounted = AccountByte();
      if (!accounted) {
        return std::unexpected(accounted.error());
      }
      state_ = CsvState::kQuoted;
    } else if (character == '\n') {
      return CsvParseAction::kFinishRecord;
    } else if (character == '\r') {
      auto consumed = ConsumeLineFeed();
      if (!consumed) {
        return std::unexpected(consumed.error());
      }
      return CsvParseAction::kFinishRecord;
    } else {
      auto accounted = AccountByte();
      if (!accounted) {
        return std::unexpected(accounted.error());
      }
      field_.push_back(character);
      state_ = CsvState::kUnquoted;
    }
    return CsvParseAction::kContinue;
  }

  CsvStepResult HandleUnquoted(char character) {
    if (character == options_.delimiter) {
      auto accounted = AccountByte();
      if (!accounted) {
        return std::unexpected(accounted.error());
      }
      auto appended = AppendField();
      if (!appended) {
        return std::unexpected(appended.error());
      }
      state_ = CsvState::kFieldStart;
    } else if (character == options_.quote) {
      return std::unexpected(MalformedError("quote inside an unquoted field"));
    } else if (character == '\n') {
      return CsvParseAction::kFinishRecord;
    } else if (character == '\r') {
      auto consumed = ConsumeLineFeed();
      if (!consumed) {
        return std::unexpected(consumed.error());
      }
      return CsvParseAction::kFinishRecord;
    } else {
      auto accounted = AccountByte();
      if (!accounted) {
        return std::unexpected(accounted.error());
      }
      field_.push_back(character);
    }
    return CsvParseAction::kContinue;
  }

  CsvStepResult HandleQuoted(char character) {
    auto accounted = AccountByte();
    if (!accounted) {
      return std::unexpected(accounted.error());
    }
    if (character == options_.quote) {
      state_ = CsvState::kAfterQuote;
    } else {
      field_.push_back(character);
    }
    return CsvParseAction::kContinue;
  }

  CsvStepResult HandleAfterQuote(char character) {
    if (character == options_.quote) {
      auto accounted = AccountByte();
      if (!accounted) {
        return std::unexpected(accounted.error());
      }
      field_.push_back(character);
      state_ = CsvState::kQuoted;
    } else if (character == options_.delimiter) {
      auto accounted = AccountByte();
      if (!accounted) {
        return std::unexpected(accounted.error());
      }
      auto appended = AppendField();
      if (!appended) {
        return std::unexpected(appended.error());
      }
      state_ = CsvState::kFieldStart;
    } else if (character == '\n') {
      return CsvParseAction::kFinishRecord;
    } else if (character == '\r') {
      auto consumed = ConsumeLineFeed();
      if (!consumed) {
        return std::unexpected(consumed.error());
      }
      return CsvParseAction::kFinishRecord;
    } else {
      return std::unexpected(MalformedError("unexpected character after a closing quote"));
    }
    return CsvParseAction::kContinue;
  }

  CsvReadResult HandleEndOfInput() {
    if (input_.bad() || (!input_.eof() && input_.fail())) {
      return StreamFailure();
    }
    if (state_ == CsvState::kQuoted) {
      return std::unexpected(MalformedError("unclosed quoted field"));
    }
    if (!saw_input_) {
      return std::optional<CsvRow>{};
    }
    return FinishRecord();
  }

  CsvReadResult HandleInputFailure(const std::ios_base::failure& failure) {
    if (input_.eof() && !input_.bad()) {
      if (state_ == CsvState::kQuoted) {
        return std::unexpected(MalformedError("unclosed quoted field"));
      }
      if (!saw_input_) {
        return std::optional<CsvRow>{};
      }
      return FinishRecord();
    }
    return StreamFailure(failure.what());
  }

  std::istream& input_;
  const CsvOptions& options_;
  Index& record_number_;
  Index current_record_;
  CsvRow fields_;
  std::string field_;
  CsvState state_ = CsvState::kFieldStart;
  std::size_t record_bytes_ = 0;
  bool saw_input_ = false;
};

}  // namespace

CsvReader::CsvReader(std::istream& input, CsvOptions options) : input_(input), options_(options) {
}

Result<std::optional<std::vector<std::string>>> CsvReader::Next() {
  auto validation = ValidateOptions(options_);
  if (!validation) {
    return std::unexpected(validation.error());
  }

  CsvRecordParser parser(input_, options_, record_number_);
  return parser.Parse();
}

}  // namespace borophene::io
