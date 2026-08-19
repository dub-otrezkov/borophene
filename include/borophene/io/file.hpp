#ifndef BOROPHENE_IO_FILE_HPP_
#define BOROPHENE_IO_FILE_HPP_

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

  [[nodiscard]] virtual Result<std::uint64_t> Size() const = 0;
  [[nodiscard]] virtual Result<std::size_t> ReadAt(std::uint64_t offset, std::span<Byte> destination) const = 0;
};

class OutputFile {
 public:
  OutputFile() = default;
  virtual ~OutputFile() = default;

  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;
  OutputFile(OutputFile&&) noexcept = default;
  OutputFile& operator=(OutputFile&&) noexcept = default;

  [[nodiscard]] virtual std::uint64_t Position() const noexcept = 0;
  [[nodiscard]] virtual Result<void> Write(std::span<const Byte> data) = 0;
  [[nodiscard]] virtual Result<void> Flush() = 0;
};

[[nodiscard]] Result<void> ReadExactly(const RandomAccessFile& file, std::uint64_t offset, std::span<Byte> destination);

[[nodiscard]] Result<std::unique_ptr<RandomAccessFile>> OpenLocalInput(const std::filesystem::path& path);
[[nodiscard]] Result<std::unique_ptr<OutputFile>> CreateLocalOutput(const std::filesystem::path& path);

}  // namespace borophene::io

#endif  // BOROPHENE_IO_FILE_HPP_
