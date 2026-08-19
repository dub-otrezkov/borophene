#include "borophene/io/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace borophene::io {

struct MemoryStream::Storage {
  std::vector<Byte> bytes;
  bool flushed = false;
};

namespace {

std::string PathError(std::string_view operation, const std::string& path, int error_number) {
  return std::string(operation) + " '" + path +
         "': " + std::error_code(error_number, std::generic_category()).message();
}

std::size_t MaximumIoSize() noexcept {
  return static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
}

bool IsValidOffset(ui64 offset) noexcept {
  return offset <= static_cast<ui64>(std::numeric_limits<off_t>::max());
}

class LocalRandomAccessFile final : public RandomAccessFile {
 public:
  LocalRandomAccessFile(int descriptor, std::string path) : descriptor_(descriptor), path_(std::move(path)) {
  }

  ~LocalRandomAccessFile() override {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  Result<ui64> Size() const override {
    struct stat status{};
    while (::fstat(descriptor_, &status) != 0) {
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      return Failure<ui64>(ErrorCode::kIo, PathError("cannot inspect", path_, error_number));
    }
    if (status.st_size < 0) {
      return Failure<ui64>(ErrorCode::kIo, "cannot inspect '" + path_ + "': negative file size");
    }
    return static_cast<ui64>(status.st_size);
  }

  Result<std::size_t> ReadAt(ui64 offset, std::span<Byte> destination) const override {
    if (destination.empty()) {
      return std::size_t{0};
    }
    if (!IsValidOffset(offset)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange,
                                  "cannot read '" + path_ + "': offset is outside the platform file range");
    }
    if (destination.size() > std::numeric_limits<ui64>::max() - offset) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange,
                                  "cannot read '" + path_ + "': byte range overflows a 64-bit offset");
    }

    const std::size_t requested = std::min(destination.size(), MaximumIoSize());
    while (true) {
      const ssize_t bytes_read = ::pread(descriptor_, destination.data(), requested, static_cast<off_t>(offset));
      if (bytes_read >= 0) {
        return static_cast<std::size_t>(bytes_read);
      }
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      return Failure<std::size_t>(ErrorCode::kIo, PathError("cannot read", path_, error_number));
    }
  }

 private:
  int descriptor_;
  std::string path_;
};

class LocalInputStream final : public InputStream {
 public:
  LocalInputStream(int descriptor, std::string path) : descriptor_(descriptor), path_(std::move(path)) {
  }

  ~LocalInputStream() override {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  Result<std::size_t> Read(std::span<Byte> destination) override {
    if (destination.empty()) {
      return std::size_t{0};
    }

    const std::size_t requested = std::min(destination.size(), MaximumIoSize());
    while (true) {
      const ssize_t bytes_read = ::read(descriptor_, destination.data(), requested);
      if (bytes_read >= 0) {
        return static_cast<std::size_t>(bytes_read);
      }
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      return Failure<std::size_t>(ErrorCode::kIo, PathError("cannot read", path_, error_number));
    }
  }

 private:
  int descriptor_;
  std::string path_;
};

class LocalOutputStream final : public OutputStream {
 public:
  LocalOutputStream(int descriptor, std::string path) : descriptor_(descriptor), path_(std::move(path)) {
  }

  ~LocalOutputStream() override {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  ui64 Position() const noexcept override {
    return position_;
  }

  Result<void> Write(std::span<const Byte> data) override {
    if (data.size() > std::numeric_limits<ui64>::max() - position_) {
      return Failure<void>(ErrorCode::kOutOfRange,
                           "cannot write '" + path_ + "': byte range overflows a 64-bit position");
    }

    std::size_t written = 0;
    while (written < data.size()) {
      const std::size_t requested = std::min(data.size() - written, MaximumIoSize());
      const ssize_t result = ::write(descriptor_, data.data() + written, requested);
      if (result > 0) {
        const auto count = static_cast<std::size_t>(result);
        written += count;
        position_ += static_cast<ui64>(count);
        continue;
      }
      if (result == 0) {
        return Failure<void>(ErrorCode::kIo, "cannot write '" + path_ + "': write made no progress");
      }
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      return Failure<void>(ErrorCode::kIo, PathError("cannot write", path_, error_number));
    }
    return {};
  }

  Result<void> Flush() override {
    while (::fsync(descriptor_) != 0) {
      const int error_number = errno;
      if (error_number == EINTR) {
        continue;
      }
      return Failure<void>(ErrorCode::kIo, PathError("cannot flush", path_, error_number));
    }
    return {};
  }

 private:
  int descriptor_;
  std::string path_;
  ui64 position_ = 0;
};

int OpenDescriptor(const std::filesystem::path& path, int flags, mode_t mode = 0) {
  while (true) {
    const int descriptor = mode == 0 ? ::open(path.c_str(), flags) : ::open(path.c_str(), flags, mode);
    if (descriptor >= 0 || errno != EINTR) {
      return descriptor;
    }
  }
}

}  // namespace

MemoryStream::MemoryStream() : storage_(std::make_shared<Storage>()) {
}

MemoryStream::MemoryStream(std::span<const Byte> data) : storage_(std::make_shared<Storage>()) {
  storage_->bytes.assign(data.begin(), data.end());
}

MemoryStream::MemoryStream(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {
}

MemoryStream::~MemoryStream() = default;

MemoryStream::MemoryStream(MemoryStream&&) noexcept = default;

MemoryStream& MemoryStream::operator=(MemoryStream&&) noexcept = default;

std::unique_ptr<MemoryStream> MemoryStream::Share() const {
  return std::unique_ptr<MemoryStream>(new MemoryStream(storage_));
}

void MemoryStream::Rewind() noexcept {
  read_position_ = 0;
}

std::span<const Byte> MemoryStream::Data() const noexcept {
  return storage_->bytes;
}

bool MemoryStream::IsFlushed() const noexcept {
  return storage_->flushed;
}

Result<std::size_t> MemoryStream::Read(std::span<Byte> destination) {
  auto read = ReadAt(read_position_, destination);
  if (!read) {
    return MakeUnexpected(std::move(read.error()));
  }
  read_position_ += static_cast<ui64>(*read);
  return *read;
}

ui64 MemoryStream::Position() const noexcept {
  return static_cast<ui64>(storage_->bytes.size());
}

Result<void> MemoryStream::Write(std::span<const Byte> data) {
  if (data.size() > std::numeric_limits<ui64>::max() - Position()) {
    return Failure<void>(ErrorCode::kOutOfRange, "memory stream size exceeds the 64-bit position range");
  }
  if (data.size() > storage_->bytes.max_size() - storage_->bytes.size()) {
    return Failure<void>(ErrorCode::kOutOfRange, "memory stream size exceeds the addressable range");
  }

  std::vector<Byte> copied_data;
  bool aliases_storage = false;
  if (!data.empty() && !storage_->bytes.empty()) {
    const Byte* storage_begin = storage_->bytes.data();
    const Byte* storage_end = storage_begin + storage_->bytes.size();
    const std::less<> less;
    aliases_storage = !less(data.data(), storage_begin) && less(data.data(), storage_end);
  }
  if (aliases_storage) {
    copied_data.assign(data.begin(), data.end());
    data = copied_data;
  }
  storage_->bytes.insert(storage_->bytes.end(), data.begin(), data.end());
  storage_->flushed = false;
  return {};
}

Result<void> MemoryStream::Flush() {
  storage_->flushed = true;
  return {};
}

Result<ui64> MemoryStream::Size() const {
  return Position();
}

Result<std::size_t> MemoryStream::ReadAt(ui64 offset, std::span<Byte> destination) const {
  if (destination.empty() || offset >= Position()) {
    return std::size_t{0};
  }

  const auto source_offset = static_cast<std::size_t>(offset);
  const std::size_t count = std::min(destination.size(), storage_->bytes.size() - source_offset);
  std::memcpy(destination.data(), storage_->bytes.data() + source_offset, count);
  return count;
}

Result<void> ReadExactly(const RandomAccessFile& file, ui64 offset, std::span<Byte> destination) {
  if (destination.size() > std::numeric_limits<ui64>::max() - offset) {
    return Failure<void>(ErrorCode::kOutOfRange, "read range overflows a 64-bit offset");
  }

  std::size_t total = 0;
  while (total < destination.size()) {
    auto result = file.ReadAt(offset + static_cast<ui64>(total), destination.subspan(total));
    if (!result) {
      return MakeUnexpected(std::move(result.error()));
    }
    if (*result == 0) {
      return Failure<void>(ErrorCode::kIo, "short read at offset " + std::to_string(offset) + ": expected " +
                                               std::to_string(destination.size()) + " bytes, received " +
                                               std::to_string(total));
    }
    if (*result > destination.size() - total) {
      return Failure<void>(ErrorCode::kIo, "file reader returned more bytes than requested");
    }
    total += *result;
  }
  return {};
}

Result<std::unique_ptr<RandomAccessFile>> OpenLocalInput(const std::filesystem::path& path) {
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const int descriptor = OpenDescriptor(path, flags);
  if (descriptor < 0) {
    const int error_number = errno;
    return Failure<std::unique_ptr<RandomAccessFile>>(ErrorCode::kIo,
                                                      PathError("cannot open", path.string(), error_number));
  }
  return std::unique_ptr<RandomAccessFile>(new LocalRandomAccessFile(descriptor, path.string()));
}

Result<std::unique_ptr<InputStream>> OpenLocalInputStream(const std::filesystem::path& path) {
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const int descriptor = OpenDescriptor(path, flags);
  if (descriptor < 0) {
    const int error_number = errno;
    return Failure<std::unique_ptr<InputStream>>(ErrorCode::kIo, PathError("cannot open", path.string(), error_number));
  }
  return std::unique_ptr<InputStream>(new LocalInputStream(descriptor, path.string()));
}

Result<std::unique_ptr<OutputStream>> CreateLocalOutput(const std::filesystem::path& path) {
  int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const int descriptor = OpenDescriptor(path, flags, static_cast<mode_t>(0666));
  if (descriptor < 0) {
    const int error_number = errno;
    return Failure<std::unique_ptr<OutputStream>>(ErrorCode::kIo,
                                                  PathError("cannot create", path.string(), error_number));
  }
  return std::unique_ptr<OutputStream>(new LocalOutputStream(descriptor, path.string()));
}

}  // namespace borophene::io
