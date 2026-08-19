#include "borophene/io/csv_reader.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace borophene::io {
namespace {

enum class CsvState : ui8 {
  kFieldStart,
  kUnquoted,
  kQuoted,
  kAfterQuote
};

enum class CsvParseAction : ui8 {
  kContinue,
  kFinishRecord
};

using CsvRow = std::vector<std::string>;
using CsvReadResult = Result<std::optional<CsvRow>>;
using CsvStepResult = Result<CsvParseAction>;
using CsvByteResult = Result<std::optional<Byte>>;

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

// Amortizes virtual backend reads while retaining unconsumed bytes between CSV records.
class BufferedInput {
 public:
  BufferedInput(InputStream& input, std::span<Byte> buffer, std::size_t& position, std::size_t& size)
      : input_(input), buffer_(buffer), position_(position), size_(size) {
  }

  CsvByteResult ReadByte() {
    if (position_ == size_) {
      auto read = input_.Read(buffer_);
      if (!read) {
        return MakeUnexpected(std::move(read.error()));
      }
      if (*read > buffer_.size()) {
        return Failure<std::optional<Byte>>(ErrorCode::kIo, "input stream returned more bytes than requested");
      }
      position_ = 0;
      size_ = *read;
    }
    if (position_ == size_) {
      return std::optional<Byte>{};
    }

    const Byte value = buffer_[position_];
    ++position_;
    return std::optional<Byte>(value);
  }

  void PutBack() noexcept {
    --position_;
  }

 private:
  InputStream& input_;
  std::span<Byte> buffer_;
  std::size_t& position_;
  std::size_t& size_;
};

class CsvRecordParser {
 public:
  CsvRecordParser(BufferedInput& input, const CsvOptions& options, Index& record_number)
      : input_(input), options_(options), record_number_(record_number), current_record_(record_number + 1) {
  }

  CsvReadResult Parse() {
    while (true) {
      auto value = ReadByte();
      if (!value) {
        return MakeUnexpected(std::move(value.error()));
      }
      if (!*value) {
        return HandleEndOfInput();
      }

      saw_input_ = true;
      const char character = static_cast<char>(std::to_integer<unsigned char>(**value));
      auto step = HandleCharacter(character);
      if (!step) {
        return MakeUnexpected(std::move(step.error()));
      }
      if (*step == CsvParseAction::kFinishRecord) {
        return FinishRecord();
      }
    }
  }

 private:
  Error MalformedError(std::string message) const {
    return {ErrorCode::kMalformedCsv, CurrentContext() + std::move(message)};
  }

  std::string CurrentContext() const {
    return CsvContext(current_record_, fields_.size() + 1);
  }

  CsvByteResult ReadByte() {
    auto value = input_.ReadByte();
    if (!value) {
      return Failure<std::optional<Byte>>(value.error().Code(), CurrentContext() + value.error().Message());
    }
    return value;
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
      return MakeUnexpected(std::move(appended.error()));
    }
    ++record_number_;
    return std::optional<CsvRow>(std::move(fields_));
  }

  Result<void> ConsumeLineFeed() {
    auto next = ReadByte();
    if (!next) {
      return MakeUnexpected(std::move(next.error()));
    }
    if (!*next) {
      return {};
    }
    if (static_cast<char>(std::to_integer<unsigned char>(**next)) != '\n') {
      input_.PutBack();
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
        return MakeUnexpected(std::move(accounted.error()));
      }
      auto appended = AppendField();
      if (!appended) {
        return MakeUnexpected(std::move(appended.error()));
      }
    } else if (character == options_.quote) {
      auto accounted = AccountByte();
      if (!accounted) {
        return MakeUnexpected(std::move(accounted.error()));
      }
      state_ = CsvState::kQuoted;
    } else if (character == '\n') {
      return CsvParseAction::kFinishRecord;
    } else if (character == '\r') {
      auto consumed = ConsumeLineFeed();
      if (!consumed) {
        return MakeUnexpected(std::move(consumed.error()));
      }
      return CsvParseAction::kFinishRecord;
    } else {
      auto accounted = AccountByte();
      if (!accounted) {
        return MakeUnexpected(std::move(accounted.error()));
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
        return MakeUnexpected(std::move(accounted.error()));
      }
      auto appended = AppendField();
      if (!appended) {
        return MakeUnexpected(std::move(appended.error()));
      }
      state_ = CsvState::kFieldStart;
    } else if (character == options_.quote) {
      return MakeUnexpected(MalformedError("quote inside an unquoted field"));
    } else if (character == '\n') {
      return CsvParseAction::kFinishRecord;
    } else if (character == '\r') {
      auto consumed = ConsumeLineFeed();
      if (!consumed) {
        return MakeUnexpected(std::move(consumed.error()));
      }
      return CsvParseAction::kFinishRecord;
    } else {
      auto accounted = AccountByte();
      if (!accounted) {
        return MakeUnexpected(std::move(accounted.error()));
      }
      field_.push_back(character);
    }
    return CsvParseAction::kContinue;
  }

  CsvStepResult HandleQuoted(char character) {
    auto accounted = AccountByte();
    if (!accounted) {
      return MakeUnexpected(std::move(accounted.error()));
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
        return MakeUnexpected(std::move(accounted.error()));
      }
      field_.push_back(character);
      state_ = CsvState::kQuoted;
    } else if (character == options_.delimiter) {
      auto accounted = AccountByte();
      if (!accounted) {
        return MakeUnexpected(std::move(accounted.error()));
      }
      auto appended = AppendField();
      if (!appended) {
        return MakeUnexpected(std::move(appended.error()));
      }
      state_ = CsvState::kFieldStart;
    } else if (character == '\n') {
      return CsvParseAction::kFinishRecord;
    } else if (character == '\r') {
      auto consumed = ConsumeLineFeed();
      if (!consumed) {
        return MakeUnexpected(std::move(consumed.error()));
      }
      return CsvParseAction::kFinishRecord;
    } else {
      return MakeUnexpected(MalformedError("unexpected character after a closing quote"));
    }
    return CsvParseAction::kContinue;
  }

  CsvReadResult HandleEndOfInput() {
    if (state_ == CsvState::kQuoted) {
      return MakeUnexpected(MalformedError("unclosed quoted field"));
    }
    if (!saw_input_) {
      return std::optional<CsvRow>{};
    }
    return FinishRecord();
  }

  BufferedInput& input_;
  const CsvOptions& options_;
  Index& record_number_;
  Index current_record_;
  CsvRow fields_;
  std::string field_;
  CsvState state_ = CsvState::kFieldStart;
  std::size_t record_bytes_ = 0;
  // Distinguishes clean EOF from a final record without a line ending.
  bool saw_input_ = false;
};

}  // namespace

CsvReader::CsvReader(InputStream& input, CsvOptions options) : input_(input), options_(options) {
}

Result<std::optional<std::vector<std::string>>> CsvReader::Next() {
  auto validation = ValidateOptions(options_);
  if (!validation) {
    return MakeUnexpected(std::move(validation.error()));
  }

  BufferedInput input(input_, input_buffer_, input_position_, input_size_);
  CsvRecordParser parser(input, options_, record_number_);
  return parser.Parse();
}

}  // namespace borophene::io
