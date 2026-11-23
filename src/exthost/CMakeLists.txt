find_package(nlohmann_json REQUIRED)

add_library(anvil_exthost STATIC
  commands_service.cpp
  contributions.cpp
  documents_service.cpp
  extension_host.cpp
  frame.cpp
  host_process.cpp
  init_data.cpp
  logging_service.cpp
  request_router.cpp
  rpc_message.cpp
  session.cpp
  session_logs.cpp
  vscode_types.cpp
)
add_library(anvil::exthost ALIAS anvil_exthost)

target_include_directories(anvil_exthost PUBLIC include)
target_link_libraries(anvil_exthost
  PUBLIC anvil::core anvil::platform nlohmann_json::nlohmann_json
  PRIVATE anvil_compile_options
)
