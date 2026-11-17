add_library(anvil_core STATIC
  char_class.cpp
  document.cpp
  editing.cpp
  log.cpp
  rope.cpp
  search.cpp
  selection.cpp
  text.cpp
  text_buffer.cpp
  text_file.cpp
  uri.cpp
  utf.cpp
)
add_library(anvil::core ALIAS anvil_core)

target_include_directories(anvil_core PUBLIC include)
# vcpkg ships a CMake package; Debian-style system packages only ship a pkg-config file.
find_package(PCRE2 CONFIG QUIET COMPONENTS 8BIT)
if(TARGET PCRE2::8BIT)
  set(ANVIL_PCRE2_TARGET PCRE2::8BIT)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)
  set(ANVIL_PCRE2_TARGET PkgConfig::PCRE2)
endif()
target_link_libraries(anvil_core PRIVATE anvil_compile_options ${ANVIL_PCRE2_TARGET})
