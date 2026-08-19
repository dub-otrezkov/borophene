function(borophene_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_23)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(BOROPHENE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(BOROPHENE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(BOROPHENE_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      message(FATAL_ERROR "Sanitizers require GCC or Clang")
    endif()
    target_compile_options(${target} PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(${target} PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
  endif()

  if(BOROPHENE_ENABLE_CLANG_TIDY)
    find_program(BOROPHENE_CLANG_TIDY_EXECUTABLE clang-tidy REQUIRED)
    set_property(
      TARGET ${target}
      PROPERTY CXX_CLANG_TIDY "${BOROPHENE_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
    )
  endif()
endfunction()

function(borophene_add_library target)
  add_library(${target} STATIC ${ARGN})
  target_include_directories(
    ${target}
    PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  )
  borophene_configure_target(${target})
endfunction()
