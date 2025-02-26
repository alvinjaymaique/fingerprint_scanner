# # The following lines of boilerplate have to be in your project's CMakeLists
# # in this exact order for cmake to work correctly
# cmake_minimum_required(VERSION 3.16)

# include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# # "Trim" the build. Include the minimal set of components, main, and anything it depends on.
# idf_build_set_property(MINIMAL_BUILD ON)
# project(uart_async_rxtxtasks)


cmake_minimum_required(VERSION 3.16)

set(PROJECT_NAME "uart_async_rxtxtasks")
project(${PROJECT_NAME})

# Include the IDF project's cmake toolchain
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Add the components directory to the build system
set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/components)

project(${PROJECT_NAME})
