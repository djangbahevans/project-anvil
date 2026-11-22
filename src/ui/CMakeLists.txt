add_library(anvil_ui STATIC
  activity_bar.cpp
  command_registry.cpp
  editor_view.cpp
  extension_host_bridge.cpp
  find_bar.cpp
  include/anvil/ui/activity_bar.hpp
  include/anvil/ui/command_registry.hpp
  include/anvil/ui/editor_view.hpp
  include/anvil/ui/find_bar.hpp
  include/anvil/ui/main_window.hpp
  include/anvil/ui/panel.hpp
  include/anvil/ui/quick_pick.hpp
  include/anvil/ui/resources.hpp
  include/anvil/ui/settings_store.hpp
  include/anvil/ui/sidebar.hpp
  include/anvil/ui/view_container_registry.hpp
  include/anvil/ui/view_container_stack.hpp
  include/anvil/ui/workbench.hpp
  include/anvil/ui/workbench_colors.hpp
  main_window.cpp
  panel.cpp
  quick_pick.cpp
  resources.cpp
  settings_store.cpp
  sidebar.cpp
  view_container_registry.cpp
  view_container_stack.cpp
  workbench.cpp
  workbench_colors.cpp
)
add_library(anvil::ui ALIAS anvil_ui)

set_target_properties(anvil_ui PROPERTIES AUTOMOC ON)
target_include_directories(anvil_ui PUBLIC include)
target_link_libraries(anvil_ui
  PUBLIC Qt6::Widgets anvil::core anvil::exthost
  PRIVATE anvil::platform anvil_compile_options
)
target_compile_definitions(anvil_ui PRIVATE ANVIL_THEMES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/themes")
