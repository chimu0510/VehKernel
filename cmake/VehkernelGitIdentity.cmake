# SPDX-License-Identifier: GPL-3.0-or-later
# The build's git identity for the firmware_info topic (ADR-0023): the commit's
# first 32 bits and whether the tree had changes not committed. A target that
# runs at every build rewrites the header only when they change, so only the
# sources that include it rebuild. Link vk_git_identity to include
# <vk_git_identity.hpp>, which declares vk::build::git_hash and vk::build::git_dirty.

set(VK_GIT_IDENTITY_DIR ${CMAKE_BINARY_DIR}/generated/git)
set(VK_GIT_IDENTITY_HEADER ${VK_GIT_IDENTITY_DIR}/vk_git_identity.hpp)

add_custom_target(vk_git_identity_header
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DHEADER=${VK_GIT_IDENTITY_HEADER}
    -P ${CMAKE_SOURCE_DIR}/cmake/git_identity.cmake
  BYPRODUCTS ${VK_GIT_IDENTITY_HEADER}
  COMMENT "Checking the git identity"
  VERBATIM)

add_library(vk_git_identity INTERFACE)
target_include_directories(vk_git_identity INTERFACE ${VK_GIT_IDENTITY_DIR})
add_dependencies(vk_git_identity vk_git_identity_header)
