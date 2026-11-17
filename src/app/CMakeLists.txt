add_executable(anvil main.cpp)
target_link_libraries(anvil PRIVATE anvil::ui anvil::platform anvil_compile_options)
target_compile_definitions(anvil PRIVATE ANVIL_DEFAULT_VSCODE_DIR="${CMAKE_SOURCE_DIR}/.deps/vscode")

if(WIN32 OR APPLE)
  set_target_properties(anvil PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
endif()
