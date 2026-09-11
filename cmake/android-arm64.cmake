# SPDX-License-Identifier: MIT
# Copyright (c) 2026 H. De Pauw

# Cross toolchain for Android arm64-v8a.
# Usage:
#   cmake -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/android-arm64.cmake \
#     -DANDROID_NDK=/path/to/android-ndk \
#     -DCMAKE_BUILD_TYPE=Release

if (NOT DEFINED ANDROID_NDK)
    message(FATAL_ERROR "ANDROID_NDK must be set to the NDK root")
endif()

if (NOT EXISTS "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    message(FATAL_ERROR "ANDROID_NDK=${ANDROID_NDK} does not look like an NDK")
endif()

include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")

set(ANDROID_ABI       arm64-v8a  CACHE STRING "" FORCE)
set(ANDROID_PLATFORM  android-21 CACHE STRING "" FORCE)
set(ANDROID_TOOLCHAIN clang      CACHE STRING "" FORCE)
set(ANDROID_STL       none       CACHE STRING "" FORCE)
