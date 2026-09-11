# SPDX-License-Identifier: MIT
# Copyright (c) 2026 H. De Pauw

# Cross toolchain for 32-bit ARM Linux (hard-float).
# Usage:
#   cmake -B build-arm \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/arm-linux-gnueabihf.cmake \
#     -DCMAKE_BUILD_TYPE=Release

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-linux-gnueabihf)

set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_AR         ${TOOLCHAIN_PREFIX}-ar     CACHE FILEPATH "")
set(CMAKE_RANLIB     ${TOOLCHAIN_PREFIX}-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP      ${TOOLCHAIN_PREFIX}-strip  CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
