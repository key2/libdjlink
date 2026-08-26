cmake_minimum_required(VERSION 3.16)
project(libdjlink C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
  -Wall -Wextra -Wpedantic -Wshadow
  -Wstrict-prototypes -Wmissing-prototypes
  -Wpointer-arith -Wcast-align
)

find_package(Threads REQUIRED)

add_library(djlink
  src/wire.c
  src/osal.c
  src/templates.c
  src/context.c
  src/dbserver.c
  src/position.c
  src/signature.c
  src/metadata.c
)
target_include_directories(djlink PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(djlink PUBLIC Threads::Threads m)
set_target_properties(djlink PROPERTIES C_VISIBILITY_PRESET default)

add_executable(djl-monitor tools/djl_monitor.c)
target_link_libraries(djl-monitor PRIVATE djlink)

add_executable(djl-meta tools/djl_meta.c)
target_link_libraries(djl-meta PRIVATE djlink)

enable_testing()
add_executable(test_wire tests/test_wire.c)
target_include_directories(test_wire PRIVATE tests)
target_link_libraries(test_wire PRIVATE djlink)
add_test(NAME wire COMMAND test_wire)

install(TARGETS djlink ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
