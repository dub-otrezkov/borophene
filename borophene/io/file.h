#pragma once

#include <memory>

namespace borophene {

class IFile {
 public:
  virtual ~IFile() = default;

  IFile(const IFile&) = delete;
  IFile& operator=(const IFile&) = delete;

  IFile(IFile&&) = delete;
  IFile& operator=(IFile&&) = delete;

  virtual int64_t GetSize() = 0;
  virtual int64_t ReadAt(int64_t position, int64_t bytes, void* output);
};

class IFileSystem {
 public:
  virtual ~IFileSystem() = default;

  IFileSystem(const IFileSystem&) = delete;
  IFileSystem& operator=(const IFileSystem&) = delete;

  IFileSystem(IFileSystem&&) = delete;
  IFileSystem& operator=(IFileSystem&&) = delete;

  virtual std::shared_ptr<IFile> OpenInputFile(const std::string& path) = 0;
};

}  // namespace borophene
