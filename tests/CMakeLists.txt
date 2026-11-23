find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(anvil_core_tests
  core/char_class_test.cpp
  core/color_theme_test.cpp
  core/document_model_test.cpp
  core/document_test.cpp
  core/editing_test.cpp
  core/editor_settings_test.cpp
  core/fuzzy_test.cpp
  core/jsonc_test.cpp
  core/key_chord_test.cpp
  core/key_dispatcher_test.cpp
  core/keybindings_test.cpp
  core/search_test.cpp
  core/theme_resources_test.cpp
  core/selection_test.cpp
  core/settings_test.cpp
  core/text_buffer_test.cpp
  core/text_file_test.cpp
  core/text_model_test.cpp
  core/text_test.cpp
  core/token_theme_test.cpp
  core/uri_test.cpp
  core/utf_test.cpp
  core/when_expression_test.cpp
)
target_link_libraries(anvil_core_tests PRIVATE anvil::core GTest::gtest_main anvil_compile_options)
target_include_directories(anvil_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_compile_definitions(anvil_core_tests PRIVATE ANVIL_CORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/data"
  ANVIL_VSCODE_DIR="${CMAKE_SOURCE_DIR}/.deps/vscode"
  ANVIL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(anvil_core_tests)

if(UNIX)
  set(ANVIL_PLATFORM_TEST_SOURCES platform/process_posix_test.cpp platform/local_socket_posix_test.cpp)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND ANVIL_PLATFORM_TEST_SOURCES platform/paths_linux_test.cpp)
  endif()
  list(APPEND ANVIL_PLATFORM_TEST_SOURCES platform/editor_defaults_test.cpp platform/file_test.cpp
       platform/working_directory_test.cpp)
  add_executable(anvil_platform_tests ${ANVIL_PLATFORM_TEST_SOURCES})
  target_link_libraries(anvil_platform_tests
    PRIVATE anvil::platform GTest::gtest_main anvil_compile_options)
  gtest_discover_tests(anvil_platform_tests)
endif()

add_executable(anvil_exthost_tests
  exthost/commands_service_test.cpp
  exthost/documents_service_test.cpp
  exthost/frame_test.cpp
  exthost/init_data_test.cpp
  exthost/logging_service_test.cpp
  exthost/replay_test.cpp
  exthost/request_router_test.cpp
  exthost/rpc_message_test.cpp
  exthost/session_logs_test.cpp
  exthost/session_test.cpp
  exthost/vscode_types_test.cpp
)
find_package(nlohmann_json REQUIRED)
target_link_libraries(anvil_exthost_tests PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(anvil_exthost_tests PRIVATE anvil::exthost GTest::gtest_main anvil_compile_options)
target_compile_definitions(anvil_exthost_tests PRIVATE ANVIL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/exthost/data")
gtest_discover_tests(anvil_exthost_tests)

add_executable(anvil_workbench_tests
  workbench/editor_layout_model_test.cpp
  workbench/editor_layout_test.cpp
)
target_link_libraries(anvil_workbench_tests
  PRIVATE anvil::workbench GTest::gtest_main anvil_compile_options)
gtest_discover_tests(anvil_workbench_tests)

find_package(nlohmann_json REQUIRED)
find_program(ANVIL_NODE node)
add_executable(anvil_exthost_integration_tests
  exthost/documents_integration_test.cpp
  exthost/extension_host_test.cpp
  exthost/host_process_test.cpp
  exthost/launch_integration_test.cpp
  exthost/logging_integration_test.cpp
)
target_link_libraries(anvil_exthost_integration_tests
  PRIVATE anvil::exthost nlohmann_json::nlohmann_json GTest::gtest_main anvil_compile_options)
target_compile_definitions(anvil_exthost_integration_tests PRIVATE
  ANVIL_NODE_EXECUTABLE="${ANVIL_NODE}"
  ANVIL_VSCODE_DIR="${CMAKE_SOURCE_DIR}/.deps/vscode"
  ANVIL_FIXTURE_EXTENSION="${CMAKE_SOURCE_DIR}/tools/exthost-probe/fixtures/hello-extension"
  ANVIL_DOCUMENTS_EXTENSION="${CMAKE_SOURCE_DIR}/tools/exthost-probe/fixtures/documents-extension"
  ANVIL_LOGGING_EXTENSION="${CMAKE_SOURCE_DIR}/tools/exthost-probe/fixtures/logging-extension"
  ANVIL_FAILING_EXTENSION="${CMAKE_SOURCE_DIR}/tools/exthost-probe/fixtures/failing-extension")
gtest_discover_tests(anvil_exthost_integration_tests)

if(TARGET anvil_ui)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(anvil_ui_tests
    ui/codicons_test.cpp
    ui/command_registry_test.cpp
    ui/default_keybindings_test.cpp
    ui/editor_area_test.cpp
    ui/editor_view_test.cpp
    ui/find_bar_test.cpp
    ui/key_translation_test.cpp
    ui/keybinding_service_test.cpp
    ui/main_window_test.cpp
    ui/panel_test.cpp
    ui/quick_pick_test.cpp
    ui/resources_test.cpp
    ui/settings_store_test.cpp
    ui/sidebar_test.cpp
    ui/splitter_test.cpp
    ui/status_bar_test.cpp
    ui/tab_strip_test.cpp
    ui/theme_store_test.cpp
    ui/view_container_registry_test.cpp
    ui/workbench_colors_test.cpp
    ui/workbench_test.cpp
    ui/ui_test_main.cpp
  )
  target_link_libraries(anvil_ui_tests
    PRIVATE anvil::ui anvil::platform Qt6::Test nlohmann_json::nlohmann_json GTest::gtest anvil_compile_options)
  target_compile_definitions(anvil_ui_tests PRIVATE
    ANVIL_NODE_EXECUTABLE="${ANVIL_NODE}"
    ANVIL_VSCODE_DIR="${CMAKE_SOURCE_DIR}/.deps/vscode"
    ANVIL_DOCUMENTS_EXTENSION="${CMAKE_SOURCE_DIR}/tools/exthost-probe/fixtures/documents-extension")
  gtest_discover_tests(anvil_ui_tests)
endif()
