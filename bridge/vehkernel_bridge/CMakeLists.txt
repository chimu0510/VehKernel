# SPDX-License-Identifier: GPL-3.0-or-later
# The Bridge node (ADR-0027, spec 0015). It builds against the generated message
# headers and the Link's framing code of the repository around it, so it needs a
# checkout of the whole repository, with this package under bridge/.
cmake_minimum_required(VERSION 3.22)
project(vehkernel_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(vehkernel_msgs REQUIRED)
# The generator needs only the standard library of the system's Python (ADR-0027).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(VK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
if(NOT EXISTS ${VK_ROOT}/tools/generators/generate_schema.py)
  message(FATAL_ERROR "vehkernel_bridge needs the Vehkernel repository around it: "
    "${VK_ROOT}/tools/generators/generate_schema.py is missing")
endif()

# Generated message headers, in the build tree and never committed (ADR-0006).
set(schema_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/schema)
set(generator ${VK_ROOT}/tools/generators/generate_schema.py)
file(GLOB messages RELATIVE ${VK_ROOT} CONFIGURE_DEPENDS ${VK_ROOT}/schema/msg/*.msg)
list(SORT messages)
file(GLOB enums RELATIVE ${VK_ROOT} CONFIGURE_DEPENDS ${VK_ROOT}/schema/enum/*.enum)
list(SORT enums)
set(profiles ${VK_ROOT}/schema/log_profiles.toml)
file(GLOB generator_sources CONFIGURE_DEPENDS
  ${VK_ROOT}/tools/generators/vkschema/*.py
  ${VK_ROOT}/tools/generators/vkboard/diagnostics.py)

set(schema_files)
foreach(file IN LISTS messages enums)
  list(APPEND schema_files ${VK_ROOT}/${file})
endforeach()

set(generated_headers
  ${schema_dir}/vk/msg/wire.hpp
  ${schema_dir}/vk/msg/enums.hpp
  ${schema_dir}/vk/msg/topics.hpp)
foreach(message IN LISTS messages)
  get_filename_component(name ${message} NAME_WE)
  list(APPEND generated_headers ${schema_dir}/vk/msg/${name}.hpp)
endforeach()

add_custom_command(
  OUTPUT ${generated_headers}
  COMMAND ${Python3_EXECUTABLE} -I ${generator} ${messages} ${enums} --profiles ${profiles} --output-dir ${schema_dir}
  # The generator leaves unchanged files alone; touch them so the command does not run again.
  COMMAND ${CMAKE_COMMAND} -E touch ${generated_headers}
  WORKING_DIRECTORY ${VK_ROOT}
  DEPENDS ${schema_files} ${profiles} ${generator} ${generator_sources}
  COMMENT "Generating topics and enums from the schema"
  VERBATIM)
add_custom_target(vehkernel_bridge_schema DEPENDS ${generated_headers})

# The Link's framing, reused from the firmware: COBS, CRC16 and frames.
add_library(vehkernel_bridge_link STATIC
  ${VK_ROOT}/modules/link/src/cobs.cpp
  ${VK_ROOT}/modules/link/src/crc16.cpp
  ${VK_ROOT}/modules/link/src/frame.cpp)
target_include_directories(vehkernel_bridge_link PUBLIC ${VK_ROOT}/modules/link/include)

# The session and the transports, free of rclcpp so that gtest can drive the session.
add_library(vehkernel_bridge_core STATIC
  src/session.cpp
  src/serial_transport.cpp
  src/udp_transport.cpp)
target_include_directories(vehkernel_bridge_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${schema_dir}>)
target_link_libraries(vehkernel_bridge_core PUBLIC vehkernel_bridge_link)
add_dependencies(vehkernel_bridge_core vehkernel_bridge_schema)

# Warnings are errors, as for the rest of Vehkernel (ADR-0018). Third-party
# headers (rclcpp's) are system includes and stay out of it.
foreach(target vehkernel_bridge_link vehkernel_bridge_core)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Werror)
endforeach()

# clang-tidy with the repository's .clang-tidy (L0, ADR-0018), as the root build's
# VK_CLANG_TIDY does; every finding is an error. Off by default: it needs clang-tidy
# installed. Only the Bridge's own sources are checked: the Link's sources are
# checked by the root build, and ROS2's headers are not ours. The root config's
# header filter does not cover bridge/, so the filter is given here, naming the
# Bridge's own headers and not the generated ones.
option(VK_CLANG_TIDY "Run clang-tidy on the Bridge's own sources" OFF)
set(bridge_tidy_targets vehkernel_bridge_core bridge)
if(BUILD_TESTING)
  list(APPEND bridge_tidy_targets test_session)
endif()

add_executable(bridge src/main.cpp src/bridge_node.cpp)
target_link_libraries(bridge PRIVATE vehkernel_bridge_core rclcpp::rclcpp)
target_compile_options(bridge PRIVATE -Wall -Wextra -Wshadow -Werror)

install(TARGETS bridge DESTINATION lib/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  # Only the gtest linters of ament, not ament_lint_auto: its copyright and style
  # checks do not know this repository's SPDX headers and style (ADR-0018).
  ament_add_gtest(test_session test/test_session.cpp)
  target_link_libraries(test_session vehkernel_bridge_core)
  target_compile_options(test_session PRIVATE -Wall -Wextra -Wshadow -Werror)
endif()

if(VK_CLANG_TIDY)
  find_program(VK_CLANG_TIDY_EXE clang-tidy REQUIRED)
  foreach(target IN LISTS bridge_tidy_targets)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY
      "${VK_CLANG_TIDY_EXE};--header-filter=.*/vehkernel_bridge/(include|src|test)/.*;--config-file=${VK_ROOT}/.clang-tidy")
  endforeach()
endif()

ament_package()
