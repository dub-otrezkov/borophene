#pragma once

namespace borophene {

class IAllocator {
 public:
  virtual ~IAllocator() = default;

  IAllocator(const IAllocator&) = delete;
  IAllocator& operator=(const IAllocator&) = delete;

  IAllocator(IAllocator&&) = delete;
  IAllocator& operator=(IAllocator&&) = delete;
};

class AllocatedData {
 public:
 private:
  
};

}  // namespace borophene
