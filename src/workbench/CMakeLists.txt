find_package(nlohmann_json REQUIRED)

add_library(anvil_workbench STATIC
  editor_layout.cpp
  include/anvil/workbench/editor_layout.hpp
  include/anvil/workbench/menu_model.hpp
  include/anvil/workbench/menu_table.hpp
  include/anvil/workbench/navigation_history.hpp
  menu_model.cpp
  navigation_history.cpp
)
add_library(anvil::workbench ALIAS anvil_workbench)

target_include_directories(anvil_workbench PUBLIC include)
target_link_libraries(anvil_workbench
  PUBLIC anvil::core anvil::exthost nlohmann_json::nlohmann_json
  PRIVATE anvil_compile_options
)
