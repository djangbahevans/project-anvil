find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(anvil_core_tests core/utf_test.cpp)
target_link_libraries(anvil_core_tests PRIVATE anvil::core GTest::gtest_main anvil_compile_options)
gtest_discover_tests(anvil_core_tests)

if(UNIX)
  set(ANVIL_PLATFORM_TEST_SOURCES platform/process_posix_test.cpp platform/local_socket_posix_test.cpp)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND ANVIL_PLATFORM_TEST_SOURCES platform/paths_linux_test.cpp)
  endif()
  add_executable(anvil_platform_tests ${ANVIL_PLATFORM_TEST_SOURCES})
  target_link_libraries(anvil_platform_tests
    PRIVATE anvil::platform GTest::gtest_main anvil_compile_options)
  gtest_discover_tests(anvil_platform_tests)
endif()

add_executable(anvil_exthost_tests
  exthost/frame_test.cpp
  exthost/replay_test.cpp
  exthost/rpc_message_test.cpp
  exthost/session_test.cpp
)
target_link_libraries(anvil_exthost_tests PRIVATE anvil::exthost GTest::gtest_main anvil_compile_options)
target_compile_definitions(anvil_exthost_tests PRIVATE ANVIL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/exthost/data")
gtest_discover_tests(anvil_exthost_tests)

find_package(nlohmann_json REQUIRED)
find_program(ANVIL_NODE node)
add_executable(anvil_exthost_integration_tests exthost/host_process_test.cpp)
target_link_libraries(anvil_exthost_integration_tests
  PRIVATE anvil::exthost nlohmann_json::nlohmann_json GTest::gtest_main anvil_compile_options)
target_compile_definitions(anvil_exthost_integration_tests PRIVATE
  ANVIL_NODE_EXECUTABLE="${ANVIL_NODE}"
  ANVIL_VSCODE_DIR="${CMAKE_SOURCE_DIR}/.deps/vscode"
  ANVIL_FIXTURE_EXTENSION="${CMAKE_SOURCE_DIR}/tools/exthost-probe/fixtures/hello-extension")
gtest_discover_tests(anvil_exthost_integration_tests)
