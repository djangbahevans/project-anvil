add_library(anvil_exthost STATIC
  frame.cpp
  rpc_message.cpp
)
add_library(anvil::exthost ALIAS anvil_exthost)

target_include_directories(anvil_exthost PUBLIC include)
target_link_libraries(anvil_exthost PUBLIC anvil::core PRIVATE anvil_compile_options)
