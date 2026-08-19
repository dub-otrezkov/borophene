cmake_minimum_required(VERSION 3.20)

project(
  borophene
  VERSION 0.1.0
  DESCRIPTION "A compact analytical columnar storage engine"
  LANGUAGES CXX
)

if(NOT UNIX)
  message(FATAL_ERROR "Borophene currently requires a POSIX-compatible local file backend")
endif()

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(BOROPHENE_IS_TOP_LEVEL ON)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
else()
  set(BOROPHENE_IS_TOP_LEVEL OFF)
endif()

option(BOROPHENE_BUILD_CLI "Build the borophene command-line application" ${BOROPHENE_IS_TOP_LEVEL})
option(BOROPHENE_BUILD_TESTS "Build the borophene test suite" ${BOROPHENE_IS_TOP_LEVEL})
option(BOROPHENE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(BOROPHENE_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(BOROPHENE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(cmake/BoropheneTarget.cmake)

add_subdirectory(src)

if(BOROPHENE_BUILD_CLI)
  add_subdirectory(app)
endif()

if(BOROPHENE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

find_program(
  BOROPHENE_CLANG_FORMAT
  NAMES clang-format clang-format-22 clang-format-21 clang-format-20 clang-format-19 clang-format-18
  HINTS /opt/homebrew/opt/llvm/bin
)
if(BOROPHENE_CLANG_FORMAT)
  file(
    GLOB_RECURSE BOROPHENE_FORMAT_FILES
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/app/*.cpp"
    "${PROJECT_SOURCE_DIR}/include/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/*.hpp"
  )

  add_custom_target(
    borophene-format-check
    COMMAND "${BOROPHENE_CLANG_FORMAT}" --dry-run --Werror ${BOROPHENE_FORMAT_FILES}
    COMMENT "Checking C++ formatting"
    VERBATIM
  )
  add_custom_target(
    borophene-format
    COMMAND "${BOROPHENE_CLANG_FORMAT}" -i ${BOROPHENE_FORMAT_FILES}
    COMMENT "Formatting C++ sources"
    VERBATIM
  )
endif()
