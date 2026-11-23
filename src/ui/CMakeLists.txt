add_library(anvil_ui STATIC
  activity_bar.cpp
  codicon_table.hpp
  codicons.cpp
  command_registry.cpp
  editor_area.cpp
  editor_view.cpp
  extension_host_bridge.cpp
  find_bar.cpp
  include/anvil/ui/activity_bar.hpp
  include/anvil/ui/codicons.hpp
  include/anvil/ui/command_registry.hpp
  include/anvil/ui/editor_area.hpp
  include/anvil/ui/editor_view.hpp
  include/anvil/ui/find_bar.hpp
  include/anvil/ui/main_window.hpp
  include/anvil/ui/panel.hpp
  include/anvil/ui/quick_pick.hpp
  include/anvil/ui/resources.hpp
  include/anvil/ui/settings_store.hpp
  include/anvil/ui/sidebar.hpp
  include/anvil/ui/splitter.hpp
  include/anvil/ui/status_bar.hpp
  include/anvil/ui/tab_strip.hpp
  include/anvil/ui/theme_store.hpp
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
  splitter.cpp
  status_bar.cpp
  tab_strip.cpp
  theme_store.cpp
  view_container_registry.cpp
  view_container_stack.cpp
  workbench.cpp
  workbench_colors.cpp
)
add_library(anvil::ui ALIAS anvil_ui)

set_target_properties(anvil_ui PROPERTIES AUTOMOC ON)
qt_add_resources(anvil_ui anvil_codicons
  PREFIX /anvil/codicons
  BASE ${CMAKE_SOURCE_DIR}/resources/codicons
  FILES ${CMAKE_SOURCE_DIR}/resources/codicons/codicon.ttf)
target_include_directories(anvil_ui PUBLIC include)
target_link_libraries(anvil_ui
  PUBLIC Qt6::Widgets anvil::core anvil::exthost anvil::workbench
  PRIVATE anvil::platform anvil_compile_options
)
target_compile_definitions(anvil_ui PRIVATE ANVIL_THEMES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/resources/themes")
