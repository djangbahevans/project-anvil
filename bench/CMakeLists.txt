find_package(benchmark REQUIRED)

add_executable(anvil_text_bench text_bench.cpp)
target_link_libraries(anvil_text_bench
  PRIVATE anvil::core benchmark::benchmark benchmark::benchmark_main anvil_compile_options)

add_executable(anvil_fuzzy_bench fuzzy_bench.cpp)
target_link_libraries(anvil_fuzzy_bench
  PRIVATE anvil::core benchmark::benchmark benchmark::benchmark_main anvil_compile_options)

add_executable(anvil_token_theme_bench token_theme_bench.cpp)
target_link_libraries(anvil_token_theme_bench
  PRIVATE anvil::core benchmark::benchmark benchmark::benchmark_main anvil_compile_options)
target_compile_definitions(anvil_token_theme_bench PRIVATE ANVIL_VSCODE_DIR="${CMAKE_SOURCE_DIR}/.deps/vscode")
