add_library(anvil_ui STATIC
  editor_view.cpp
  extension_host_bridge.cpp
  include/anvil/ui/editor_view.hpp
  include/anvil/ui/main_window.hpp
  main_window.cpp
)
add_library(anvil::ui ALIAS anvil_ui)

set_target_properties(anvil_ui PROPERTIES AUTOMOC ON)
target_include_directories(anvil_ui PUBLIC include)
target_link_libraries(anvil_ui
  PUBLIC Qt6::Widgets anvil::core anvil::exthost
  PRIVATE anvil::platform anvil_compile_options
)
