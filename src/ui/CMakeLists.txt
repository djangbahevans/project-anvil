add_library(anvil_ui STATIC
  include/anvil/ui/main_window.hpp
  main_window.cpp
)
add_library(anvil::ui ALIAS anvil_ui)

set_target_properties(anvil_ui PROPERTIES AUTOMOC ON)
target_include_directories(anvil_ui PUBLIC include)
target_link_libraries(anvil_ui
  PUBLIC Qt6::Widgets
  PRIVATE anvil::core anvil::platform anvil_compile_options
)
