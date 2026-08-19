#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#include "borophene/common/result.hpp"
#include "borophene/common/types.hpp"

namespace borophene::io {

class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  virtual ~RandomAccessFile() = default;

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  RandomAccessFile(RandomAccessFile&&) noexcept = default;
  RandomAccessFile& operator=(RandomAccessFile&&) noexcept = default;

  virtual Result<std::uint64_t> Size() const = 0;
  virtual Result<std::size_t> ReadAt(std::uint64_t offset, std::span<Byte> destination) const = 0;
};

class OutputFile {
 public:
  OutputFile() = default;
  virtual ~OutputFile() = default;

  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;
  OutputFile(OutputFile&&) noexcept = default;
  OutputFile& operator=(OutputFile&&) noexcept = default;

  virtual std::uint64_t Position() const noexcept = 0;
  virtual Result<void> Write(std::span<const Byte> data) = 0;
  virtual Result<void> Flush() = 0;
};

Result<void> ReadExactly(const RandomAccessFile& file, std::uint64_t offset, std::span<Byte> destination);

Result<std::unique_ptr<RandomAccessFile>> OpenLocalInput(const std::filesystem::path& path);
Result<std::unique_ptr<OutputFile>> CreateLocalOutput(const std::filesystem::path& path);

}  // namespace borophene::io
