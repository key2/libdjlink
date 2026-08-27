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

# The NFS client is the only optional feature that needs sockets beyond the four
# DJ Link UDP ports. The PDB and ANLZ readers are pure and always built, since
# they are equally useful against a local rekordbox export archive.
option(DJL_WITH_NFS "Build the NFS/portmap/mount client for reading player media" ON)

add_library(djlink
  src/wire.c
  src/osal.c
  src/templates.c
  src/context.c
  src/dbserver.c
  src/position.c
  src/signature.c
  src/metadata.c
  src/pdb/pdb.c
  src/anlz/anlz.c
)

if(DJL_WITH_NFS)
  target_sources(djlink PRIVATE
    src/nfs/rpc.c
    src/nfs/nfs2.c
    src/nfs/fetcher.c
  )
  target_compile_definitions(djlink PUBLIC DJL_WITH_NFS=1)
else()
  target_sources(djlink PRIVATE src/nfs/nfs_stub.c)
endif()

target_include_directories(djlink PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(djlink PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(djlink PUBLIC Threads::Threads m)
set_target_properties(djlink PROPERTIES C_VISIBILITY_PRESET default)

add_executable(djl-monitor tools/djl_monitor.c)
target_link_libraries(djl-monitor PRIVATE djlink)

add_executable(djl-meta tools/djl_meta.c)
target_link_libraries(djl-meta PRIVATE djlink)

add_executable(djl-nfs tools/djl_nfs.c)
target_link_libraries(djl-nfs PRIVATE djlink)

enable_testing()
add_executable(test_wire tests/test_wire.c tests/test_nfs.c tests/test_position.c tests/test_rblink.c)
target_include_directories(test_wire PRIVATE tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_wire PRIVATE djlink)
add_test(NAME wire COMMAND test_wire)

install(TARGETS djlink ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
