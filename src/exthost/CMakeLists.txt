add_library(anvil_exthost STATIC
  frame.cpp
  host_process.cpp
  rpc_message.cpp
  session.cpp
)
add_library(anvil::exthost ALIAS anvil_exthost)

target_include_directories(anvil_exthost PUBLIC include)
target_link_libraries(anvil_exthost PUBLIC anvil::core anvil::platform PRIVATE anvil_compile_options)
