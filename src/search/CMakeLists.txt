add_library(anvil_search STATIC
  include/anvil/search/ripgrep.hpp
  ripgrep.cpp
)
add_library(anvil::search ALIAS anvil_search)

target_include_directories(anvil_search PUBLIC include)
target_link_libraries(anvil_search
  PUBLIC anvil::core anvil::platform
  PRIVATE anvil_compile_options
)
