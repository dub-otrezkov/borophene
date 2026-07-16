#pragma once

#include <stdexcept>
#include <string>

inline void ThrowAssertError(const char* file, int line, const char* cond, const std::string& message = "") {
  std::string err = std::string(file) + ":" + std::to_string(line) + ": condition '" + cond + "' is not satisfied";
  if (!message.empty()) {
    err += ": " + message;
  }
  throw std::runtime_error(err);
}

#ifdef NDEBUG

#define ASSERT(cond) ((void)0)
#define ASSERT_WITH_MESSAGE(cond, message) ((void)0)

#else

#define ASSERT(cond)                               \
  do {                                             \
    if (!cond) {                                   \
      ThrowAssertError(__FILE__, __LINE__, #cond); \
    }                                              \
  } while (false)

#define ASSERT_WITH_MESSAGE(cond, message)                    \
  do {                                                        \
    if (!cond) {                                              \
      ThrowAssertError(__FILE__, __LINE__, #cond, (message)); \
    }                                                         \
  } while (false)

#endif

#define THROW_NOT_IMPLEMENTED \
  throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": not implemented")

#define THROW_RUNTIME_ERROR(msg) \
  throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + (msg))
