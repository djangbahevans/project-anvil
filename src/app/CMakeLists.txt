add_executable(anvil main.cpp)
target_link_libraries(anvil PRIVATE anvil::ui anvil_compile_options)

if(WIN32 OR APPLE)
  set_target_properties(anvil PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
endif()
