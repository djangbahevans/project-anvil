add_library(anvil_core STATIC
  brackets.cpp
  capture_scopes.cpp
  char_class.cpp
  color_theme.cpp
  document.cpp
  editing.cpp
  occurrences.cpp
  editor_settings.cpp
  file_name_order.cpp
  fuzzy.cpp
  glob.cpp
  go_to_line.cpp
  comment_commands.cpp
  language_comments.cpp
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

  # Compiles the parser (and scanner, when there is one) of the grammar in `dir` under
  # .deps/tree-sitter, the directory that holds `src/parser.c`.
  function(anvil_add_tree_sitter_parser dir)
    set(grammar_dir "${ANVIL_TREE_SITTER_DIR}/${dir}")
    set(sources "${grammar_dir}/src/parser.c")
    if(EXISTS "${grammar_dir}/src/scanner.c")
      list(APPEND sources "${grammar_dir}/src/scanner.c")
    endif()
    set_source_files_properties(${sources} PROPERTIES
      COMPILE_OPTIONS "${ANVIL_THIRD_PARTY_C_OPTIONS}"
      INCLUDE_DIRECTORIES "${grammar_dir}/src")
    target_sources(anvil_core PRIVATE ${sources})
  endfunction()

  # Embeds the highlights query of a language as `anvil::core::grammar_queries::<language>`.
  #   OVERLAYS  the files `queries/<overlay>.scm` of this directory: Anvil's own patterns
  #   UPSTREAM  `highlights.scm` files of the grammars, as paths under .deps/tree-sitter
  # The query is the overlays and then the upstream files, each in the order given. Where two
  # patterns capture the same node the first one wins, so Anvil's patterns decide the nodes that
  # they name, and a language that builds on another lists its own files before those it takes
  # from the other, so that it can refine them.
  function(anvil_add_highlights_query language)
    cmake_parse_arguments(ARG "" "" "OVERLAYS;UPSTREAM" ${ARGN})
    set(query "")
    foreach(overlay ${ARG_OVERLAYS})
      set(file "${CMAKE_CURRENT_SOURCE_DIR}/queries/${overlay}.scm")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
      file(READ "${file}" part)
      string(APPEND query "${part}\n")
    endforeach()
    foreach(upstream ${ARG_UPSTREAM})
      set(file "${ANVIL_TREE_SITTER_DIR}/${upstream}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
      file(READ "${file}" part)
      string(APPEND query "${part}\n")
    endforeach()

    set(ANVIL_HIGHLIGHTS_QUERY "${query}")
    set(ANVIL_GRAMMAR_NAME "${language}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/grammar_query_${language}.cpp")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/grammar_query.cpp.in" "${generated}" @ONLY)
    target_sources(anvil_core PRIVATE "${generated}")
  endfunction()

  target_sources(anvil_core PRIVATE ${ANVIL_TREE_SITTER_SOURCES} tree_sitter_highlighter.cpp
    highlight_worker.cpp)
  foreach(dir json c cpp javascript typescript/typescript typescript/tsx markdown/tree-sitter-markdown
                  markdown/tree-sitter-markdown-inline)
    anvil_add_tree_sitter_parser("${dir}")
  endforeach()

  anvil_add_highlights_query(json UPSTREAM json/queries/highlights.scm)
  anvil_add_highlights_query(c OVERLAYS c UPSTREAM c/queries/highlights.scm)
  anvil_add_highlights_query(cpp OVERLAYS cpp c
    UPSTREAM cpp/queries/highlights.scm c/queries/highlights.scm)
  anvil_add_highlights_query(javascript OVERLAYS javascript javascript-jsx
    UPSTREAM javascript/queries/highlights.scm javascript/queries/highlights-jsx.scm)
  anvil_add_highlights_query(typescript OVERLAYS typescript javascript
    UPSTREAM typescript/queries/highlights.scm javascript/queries/highlights.scm)
  anvil_add_highlights_query(typescriptreact OVERLAYS typescript javascript javascript-jsx
    UPSTREAM typescript/queries/highlights.scm javascript/queries/highlights-jsx.scm
             javascript/queries/highlights.scm)
  anvil_add_highlights_query(markdown OVERLAYS markdown
    UPSTREAM markdown/tree-sitter-markdown/queries/highlights.scm)
  anvil_add_highlights_query(markdown_inline OVERLAYS markdown-inline
    UPSTREAM markdown/tree-sitter-markdown-inline/queries/highlights.scm)
  target_include_directories(anvil_core PRIVATE "${ANVIL_TREE_SITTER_RUNTIME}/include")
  target_compile_definitions(anvil_core PUBLIC ANVIL_TREE_SITTER)
endif()
