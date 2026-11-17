find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(anvil_core_tests core/utf_test.cpp)
target_link_libraries(anvil_core_tests PRIVATE anvil::core GTest::gtest_main anvil_compile_options)
gtest_discover_tests(anvil_core_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(anvil_platform_tests platform/paths_linux_test.cpp)
  target_link_libraries(anvil_platform_tests
    PRIVATE anvil::platform GTest::gtest_main anvil_compile_options)
  gtest_discover_tests(anvil_platform_tests)
endif()
