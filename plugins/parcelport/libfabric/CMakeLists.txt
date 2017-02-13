
find_package(Libfabric REQUIRED)
include_directories(${LIBFABRIC_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(fabric_pingpong
  fabric_pingpong.c
)

target_link_libraries(fabric_pingpong
  ${LIBFABRIC_LIBRARIES}
)