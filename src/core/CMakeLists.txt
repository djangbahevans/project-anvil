add_library(anvil_core STATIC
  brackets.cpp
  capture_scopes.cpp
  char_class.cpp
  color_theme.cpp
  document.cpp
  editing.cpp
  occurrences.cpp
  editor_settings.cpp
  fuzzy.cpp
  glob.cpp
  go_to_line.cpp
  language_registry.cpp
  live_highlighter.cpp
  jsonc.cpp
  jsonc_edit.cpp
  key_chord.cpp
  line_operations.cpp
  multi_cursor.cpp
  key_dispatcher.cpp
  keybindings.cpp
  log.cpp
  rope.cpp
  search.cpp
  selection.cpp
  smart_select.cpp
  settings.cpp
  text.cpp
  text_buffer.cpp
  text_file.cpp
  token_theme.cpp
  uri.cpp
  workspace.cpp
  utf.cpp
  when_expression.cpp
  whitespace.cpp
)
add_library(anvil::core ALIAS anvil_core)

find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
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
target_link_libraries(anvil_core
  PUBLIC nlohmann_json::nlohmann_json Threads::Threads
  PRIVATE anvil_compile_options ${ANVIL_PCRE2_TARGET}
)

if(ANVIL_HAVE_TREE_SITTER)
  # Third-party C code is compiled with its own warnings off, and only those files.
  if(MSVC)
    set(ANVIL_THIRD_PARTY_C_OPTIONS /W0)
  else()
    set(ANVIL_THIRD_PARTY_C_OPTIONS -w)
  endif()

  set(ANVIL_TREE_SITTER_RUNTIME "${ANVIL_TREE_SITTER_DIR}/runtime/lib")
  set(ANVIL_TREE_SITTER_SOURCES "${ANVIL_TREE_SITTER_RUNTIME}/src/lib.c")
  set_source_files_properties("${ANVIL_TREE_SITTER_RUNTIME}/src/lib.c" PROPERTIES
    COMPILE_OPTIONS "${ANVIL_THIRD_PARTY_C_OPTIONS}"
    INCLUDE_DIRECTORIES "${ANVIL_TREE_SITTER_RUNTIME}/src;${ANVIL_TREE_SITTER_RUNTIME}/include"
    COMPILE_DEFINITIONS "_POSIX_C_SOURCE=200112L;_DEFAULT_SOURCE;_DARWIN_C_SOURCE")

  # One call per grammar in .deps/tree-sitter/<name>: compiles its parser (and scanner, when it
  # has one) and embeds its highlights query as `anvil::core::grammar_queries::<name>`.
  function(anvil_add_tree_sitter_grammar name)
    set(grammar_dir "${ANVIL_TREE_SITTER_DIR}/${name}")
    set(sources "${grammar_dir}/src/parser.c")
    if(EXISTS "${grammar_dir}/src/scanner.c")
      list(APPEND sources "${grammar_dir}/src/scanner.c")
    endif()
    set_source_files_properties(${sources} PROPERTIES
      COMPILE_OPTIONS "${ANVIL_THIRD_PARTY_C_OPTIONS}"
      INCLUDE_DIRECTORIES "${grammar_dir}/src")

    set(query_file "${grammar_dir}/queries/highlights.scm")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${query_file}")
    file(READ "${query_file}" ANVIL_HIGHLIGHTS_QUERY)
    set(ANVIL_GRAMMAR_NAME "${name}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/grammar_query_${name}.cpp")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/grammar_query.cpp.in" "${generated}" @ONLY)

    target_sources(anvil_core PRIVATE ${sources} "${generated}")
  endfunction()

  target_sources(anvil_core PRIVATE ${ANVIL_TREE_SITTER_SOURCES} tree_sitter_highlighter.cpp highlight_worker.cpp)
  anvil_add_tree_sitter_grammar(json)
  target_include_directories(anvil_core PRIVATE "${ANVIL_TREE_SITTER_RUNTIME}/include")
  target_compile_definitions(anvil_core PUBLIC ANVIL_TREE_SITTER)
endif()
