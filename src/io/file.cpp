#include "borophene/io/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace borophene::io {
namespace {

[[nodiscard]] std::string PathError(std::string_view operation, const std::string& path, int error_number) {
  return std::string(operation) + " '" + path +
         "': " + std::error_code(error_number, std::generic_category()).message();
}

[[nodiscard]] std::size_t MaximumIoSize() noexcept {
  return static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
}

[[nodiscard]] bool IsValidOffset(std::uint64_t offset) noexcept {
  return offset <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
}

class LocalRandomAccessFile final : public RandomAccessFile {
 public:
  LocalRandomAccessFile(int descriptor, std::string path) : descriptor_(descriptor), path_(std::move(path)) {}

  ~LocalRandomAccessFile() override {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  [[nodiscard]] Result<std::uint64_t> Size() const override {
    struct stat status{};
    while (::fstat(descriptor_, &status) != 0) {
      const int error_number = errno;
      if (error_number == EINTR) continue;
      return Failure<std::uint64_t>(ErrorCode::kIo, PathError("cannot inspect", path_, error_number));
    }
    if (status.st_size < 0) {
      return Failure<std::uint64_t>(ErrorCode::kIo, "cannot inspect '" + path_ + "': negative file size");
    }
    return static_cast<std::uint64_t>(status.st_size);
  }

  [[nodiscard]] Result<std::size_t> ReadAt(std::uint64_t offset, std::span<Byte> destination) const override {
    if (destination.empty()) return std::size_t{0};
    if (!IsValidOffset(offset)) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange,
                                  "cannot read '" + path_ + "': offset is outside the platform file range");
    }
    if (destination.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
      return Failure<std::size_t>(ErrorCode::kOutOfRange,
                                  "cannot read '" + path_ + "': byte range overflows a 64-bit offset");
    }

    const std::size_t requested = std::min(destination.size(), MaximumIoSize());
    while (true) {
      const ssize_t bytes_read = ::pread(descriptor_, destination.data(), requested, static_cast<off_t>(offset));
      if (bytes_read >= 0) return static_cast<std::size_t>(bytes_read);
      const int error_number = errno;
      if (error_number == EINTR) continue;
      return Failure<std::size_t>(ErrorCode::kIo, PathError("cannot read", path_, error_number));
    }
  }

 private:
  int descriptor_;
  std::string path_;
};

class LocalOutputFile final : public OutputFile {
 public:
  LocalOutputFile(int descriptor, std::string path) : descriptor_(descriptor), path_(std::move(path)) {}

  ~LocalOutputFile() override {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  [[nodiscard]] std::uint64_t Position() const noexcept override { return position_; }

  [[nodiscard]] Result<void> Write(std::span<const Byte> data) override {
    if (data.size() > std::numeric_limits<std::uint64_t>::max() - position_) {
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
        position_ += static_cast<std::uint64_t>(count);
        continue;
      }
      if (result == 0) {
        return Failure<void>(ErrorCode::kIo, "cannot write '" + path_ + "': write made no progress");
      }
      const int error_number = errno;
      if (error_number == EINTR) continue;
      return Failure<void>(ErrorCode::kIo, PathError("cannot write", path_, error_number));
    }
    return {};
  }

  [[nodiscard]] Result<void> Flush() override {
    while (::fsync(descriptor_) != 0) {
      const int error_number = errno;
      if (error_number == EINTR) continue;
      return Failure<void>(ErrorCode::kIo, PathError("cannot flush", path_, error_number));
    }
    return {};
  }

 private:
  int descriptor_;
  std::string path_;
  std::uint64_t position_ = 0;
};

[[nodiscard]] int OpenDescriptor(const std::filesystem::path& path, int flags, mode_t mode = 0) {
  while (true) {
    const int descriptor = mode == 0 ? ::open(path.c_str(), flags) : ::open(path.c_str(), flags, mode);
    if (descriptor >= 0 || errno != EINTR) return descriptor;
  }
}

}  // namespace

Result<void> ReadExactly(const RandomAccessFile& file, std::uint64_t offset, std::span<Byte> destination) {
  if (destination.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
    return Failure<void>(ErrorCode::kOutOfRange, "read range overflows a 64-bit offset");
  }

  std::size_t total = 0;
  while (total < destination.size()) {
    auto result = file.ReadAt(offset + static_cast<std::uint64_t>(total), destination.subspan(total));
    if (!result) return std::unexpected(result.error());
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

Result<std::unique_ptr<OutputFile>> CreateLocalOutput(const std::filesystem::path& path) {
  int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  const int descriptor = OpenDescriptor(path, flags, static_cast<mode_t>(0666));
  if (descriptor < 0) {
    const int error_number = errno;
    return Failure<std::unique_ptr<OutputFile>>(ErrorCode::kIo,
                                                PathError("cannot create", path.string(), error_number));
  }
  return std::unique_ptr<OutputFile>(new LocalOutputFile(descriptor, path.string()));
}

}  // namespace borophene::io
