function(borophene_add_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE ${ARGN})
  borophene_configure_target(${target})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

borophene_add_test(borophene_common_data_test common_data_test.cpp borophene::data)
borophene_add_test(borophene_csv_io_test csv_io_test.cpp borophene::io)
borophene_add_test(borophene_columnar_io_test columnar_io_test.cpp borophene::storage)
borophene_add_test(borophene_execution_test execution_test.cpp borophene::execution)

file(
  GLOB_RECURSE BOROPHENE_PUBLIC_HEADERS
  CONFIGURE_DEPENDS
  RELATIVE "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/include/borophene/*.hpp"
)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/public_headers")
set(BOROPHENE_PUBLIC_HEADER_SOURCES)
foreach(BOROPHENE_PUBLIC_HEADER IN LISTS BOROPHENE_PUBLIC_HEADERS)
  string(MAKE_C_IDENTIFIER "${BOROPHENE_PUBLIC_HEADER}" BOROPHENE_PUBLIC_HEADER_ID)
  set(BOROPHENE_PUBLIC_HEADER_SOURCE
      "${CMAKE_CURRENT_BINARY_DIR}/public_headers/${BOROPHENE_PUBLIC_HEADER_ID}.cpp"
  )
  configure_file(public_header_test.cpp.in "${BOROPHENE_PUBLIC_HEADER_SOURCE}" @ONLY)
  list(APPEND BOROPHENE_PUBLIC_HEADER_SOURCES "${BOROPHENE_PUBLIC_HEADER_SOURCE}")
endforeach()
add_library(borophene_public_headers_test OBJECT ${BOROPHENE_PUBLIC_HEADER_SOURCES})
target_link_libraries(borophene_public_headers_test PRIVATE borophene::borophene)
borophene_configure_target(borophene_public_headers_test)

if(BOROPHENE_BUILD_CLI)
  add_test(NAME borophene_cli_help COMMAND borophene_cli --help)
  add_test(NAME borophene_cli_version COMMAND borophene_cli --version)
endif()
