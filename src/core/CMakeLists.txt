add_library(anvil_core STATIC
  rope.cpp
  text.cpp
  text_buffer.cpp
  utf.cpp
)
add_library(anvil::core ALIAS anvil_core)

target_include_directories(anvil_core PUBLIC include)
target_link_libraries(anvil_core PRIVATE anvil_compile_options)
