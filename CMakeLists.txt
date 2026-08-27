cmake_minimum_required(VERSION 3.20)
project(screenshot VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
  x11        # window creation, event loop, XGetImage
  xext       # XShape - punches the see-through hole
  xcursor    # resize cursors on the drag handles
  cairo      # draws the UI and writes the PNG
  cairo-xlib # cairo rendering straight onto the X window
)

add_executable(screenshot
  src/main.cpp
  src/options.cpp
)

target_compile_options(screenshot PRIVATE -Wall -Wextra)
target_link_libraries(screenshot PRIVATE PkgConfig::DEPS)

install(TARGETS screenshot RUNTIME DESTINATION bin)
