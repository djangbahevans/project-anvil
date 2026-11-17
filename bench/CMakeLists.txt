find_package(benchmark REQUIRED)

add_executable(anvil_text_bench text_bench.cpp)
target_link_libraries(anvil_text_bench
  PRIVATE anvil::core benchmark::benchmark benchmark::benchmark_main anvil_compile_options)
