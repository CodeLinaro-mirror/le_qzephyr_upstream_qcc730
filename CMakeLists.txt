# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0;

# Apply Zephyr source patches before any Zephyr configuration
# This ensures patches are applied before Zephyr is configured and built
message(STATUS "DEBUG: Checking patch application status...")
message(STATUS "DEBUG: ZEPHYR_PATCHES_APPLIED = ${ZEPHYR_PATCHES_APPLIED}")
message(STATUS "DEBUG: CMAKE_CURRENT_LIST_DIR = ${CMAKE_CURRENT_LIST_DIR}")

if(NOT ZEPHYR_PATCHES_APPLIED)
  set(PATCH_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../qapp/patch/apply_patches.py")
  message(STATUS "DEBUG: PATCH_SCRIPT = ${PATCH_SCRIPT}")
  message(STATUS "DEBUG: Checking if patch script exists...")
  
  if(EXISTS "${PATCH_SCRIPT}")
    message(STATUS "DEBUG: Patch script found, proceeding with patch application...")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    message(STATUS "DEBUG: Python3_EXECUTABLE = ${Python3_EXECUTABLE}")
    message(STATUS "DEBUG: Working directory = ${CMAKE_CURRENT_LIST_DIR}")
    message(STATUS "Applying Zephyr source patches...")
    
    execute_process(
      COMMAND ${Python3_EXECUTABLE} "${PATCH_SCRIPT}" apply
      WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
      RESULT_VARIABLE PATCH_RESULT
      OUTPUT_VARIABLE PATCH_OUTPUT
      ERROR_VARIABLE PATCH_ERROR
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )
    
    message(STATUS "DEBUG: PATCH_RESULT = ${PATCH_RESULT}")
    message(STATUS "DEBUG: PATCH_OUTPUT = ${PATCH_OUTPUT}")
    if(PATCH_ERROR)
      message(STATUS "DEBUG: PATCH_ERROR = ${PATCH_ERROR}")
    endif()
    
    if(PATCH_RESULT EQUAL 0)
      message(STATUS "Zephyr patches applied successfully")
      set(ZEPHYR_PATCHES_APPLIED TRUE CACHE INTERNAL "Zephyr patches have been applied")
    else()
      message(WARNING "Failed to apply Zephyr patches:")
      message(WARNING "  Return code: ${PATCH_RESULT}")
      message(WARNING "  Output: ${PATCH_OUTPUT}")
      message(WARNING "  Error: ${PATCH_ERROR}")
    endif()
  else()
    message(WARNING "DEBUG: Patch script not found at: ${PATCH_SCRIPT}")
    message(STATUS "DEBUG: Listing contents of qapp directory...")
    file(GLOB QAPP_CONTENTS "${CMAKE_CURRENT_LIST_DIR}/../qapp/*")
    foreach(item ${QAPP_CONTENTS})
      message(STATUS "DEBUG:   ${item}")
    endforeach()
  endif()
else()
  message(STATUS "DEBUG: Patches already applied, skipping...")
endif()

zephyr_include_directories(include)

add_subdirectory(drivers)
