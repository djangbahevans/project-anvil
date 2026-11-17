add_library(anvil_compile_options INTERFACE)

if(MSVC)
  target_compile_options(anvil_compile_options INTERFACE /W4 /permissive- /utf-8)
  if(ANVIL_WARNINGS_AS_ERRORS)
    target_compile_options(anvil_compile_options INTERFACE /WX)
  endif()
else()
  target_compile_options(anvil_compile_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion)
  if(ANVIL_WARNINGS_AS_ERRORS)
    target_compile_options(anvil_compile_options INTERFACE -Werror)
  endif()
  # libstdc++ only provides <expected> when __cpp_concepts >= 202002L, but Clang reports 201907L
  # even though it implements the required concepts support.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(anvil_compile_options INTERFACE
      -D__cpp_concepts=202002L -Wno-builtin-macro-redefined)
  endif()
  if(ANVIL_SANITIZE)
    target_compile_options(anvil_compile_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(anvil_compile_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()
