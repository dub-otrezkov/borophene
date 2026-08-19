#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"

namespace borophene::io {

class InputStream {
 public:
  InputStream() = default;
  virtual ~InputStream() = default;

  InputStream(const InputStream&) = delete;
  InputStream& operator=(const InputStream&) = delete;
  InputStream(InputStream&&) noexcept = default;
  InputStream& operator=(InputStream&&) noexcept = default;

  // Returns zero at end of input; otherwise reads at most destination.size() bytes.
  virtual Result<std::size_t> Read(std::span<Byte> destination) = 0;
};

class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  virtual ~RandomAccessFile() = default;

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  RandomAccessFile(RandomAccessFile&&) noexcept = default;
  RandomAccessFile& operator=(RandomAccessFile&&) noexcept = default;

  virtual Result<ui64> Size() const = 0;
  virtual Result<std::size_t> ReadAt(ui64 offset, std::span<Byte> destination) const = 0;
};

class OutputStream {
 public:
  OutputStream() = default;
  virtual ~OutputStream() = default;

  OutputStream(const OutputStream&) = delete;
  OutputStream& operator=(const OutputStream&) = delete;
  OutputStream(OutputStream&&) noexcept = default;
  OutputStream& operator=(OutputStream&&) noexcept = default;

  // Returns the logical byte count accepted by successful writes.
  virtual ui64 Position() const noexcept = 0;
  // Writes the complete span and advances Position() by data.size(), or returns an error.
  virtual Result<void> Write(std::span<const Byte> data) = 0;
  virtual Result<void> Flush() = 0;
};

class MemoryStream final : public InputStream, public OutputStream, public RandomAccessFile {
 public:
  MemoryStream();
  explicit MemoryStream(std::span<const Byte> data);
  ~MemoryStream() override;

  MemoryStream(const MemoryStream&) = delete;
  MemoryStream& operator=(const MemoryStream&) = delete;
  MemoryStream(MemoryStream&&) noexcept;
  MemoryStream& operator=(MemoryStream&&) noexcept;

  // Shares bytes and flush state while keeping an independent sequential read cursor.
  std::unique_ptr<MemoryStream> Share() const;
  void Rewind() noexcept;
  std::span<const Byte> Data() const noexcept;
  bool IsFlushed() const noexcept;

  Result<std::size_t> Read(std::span<Byte> destination) override;
  ui64 Position() const noexcept override;
  Result<void> Write(std::span<const Byte> data) override;
  Result<void> Flush() override;
  Result<ui64> Size() const override;
  Result<std::size_t> ReadAt(ui64 offset, std::span<Byte> destination) const override;

 private:
  struct Storage;

  explicit MemoryStream(std::shared_ptr<Storage> storage);

  std::shared_ptr<Storage> storage_;
  ui64 read_position_ = 0;
};

Result<void> ReadExactly(const RandomAccessFile& file, ui64 offset, std::span<Byte> destination);

Result<std::unique_ptr<RandomAccessFile>> OpenLocalInput(const std::filesystem::path& path);
Result<std::unique_ptr<InputStream>> OpenLocalInputStream(const std::filesystem::path& path);
Result<std::unique_ptr<OutputStream>> CreateLocalOutput(const std::filesystem::path& path);

}  // namespace borophene::io
