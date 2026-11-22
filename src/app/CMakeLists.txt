add_executable(anvil main.cpp)
target_link_libraries(anvil PRIVATE anvil::ui anvil::platform anvil_compile_options)
target_compile_definitions(anvil PRIVATE ANVIL_DEFAULT_VSCODE_DIR="${CMAKE_SOURCE_DIR}/.deps/vscode")

if(WIN32 OR APPLE)
  set_target_properties(anvil PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
endif()

# The layout of an installed editor: the executable in bin, and what it ships with in share, which
# ui::themes_directory finds from the executable's directory.
install(TARGETS anvil RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/resources/themes/ DESTINATION share/anvil/themes)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        DESTINATION share/doc/anvil)
