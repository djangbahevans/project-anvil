add_library(anvil_ui STATIC
  command_registry.cpp
  editor_view.cpp
  extension_host_bridge.cpp
  find_bar.cpp
  include/anvil/ui/command_registry.hpp
  include/anvil/ui/editor_view.hpp
  include/anvil/ui/find_bar.hpp
  include/anvil/ui/main_window.hpp
  include/anvil/ui/quick_pick.hpp
  include/anvil/ui/settings_store.hpp
  include/anvil/ui/workbench_colors.hpp
  main_window.cpp
  quick_pick.cpp
  settings_store.cpp
  workbench_colors.cpp
)
add_library(anvil::ui ALIAS anvil_ui)

set_target_properties(anvil_ui PROPERTIES AUTOMOC ON)
target_include_directories(anvil_ui PUBLIC include)
target_link_libraries(anvil_ui
  PUBLIC Qt6::Widgets anvil::core anvil::exthost
  PRIVATE anvil::platform anvil_compile_options
)
