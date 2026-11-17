if(WIN32)
  set(ANVIL_PLATFORM_SOURCES paths_windows.cpp)
elseif(APPLE)
  set(ANVIL_PLATFORM_SOURCES paths_macos.cpp)
elseif(UNIX)
  set(ANVIL_PLATFORM_SOURCES paths_linux.cpp)
else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

add_library(anvil_platform STATIC ${ANVIL_PLATFORM_SOURCES})
add_library(anvil::platform ALIAS anvil_platform)

target_include_directories(anvil_platform PUBLIC include)
target_link_libraries(anvil_platform PUBLIC anvil::core PRIVATE anvil_compile_options)

if(WIN32)
  target_link_libraries(anvil_platform PRIVATE shell32 ole32)
endif()
