# Mira Android arm64-v8a baseline. The NDK is supplied by the host or CI via
# ANDROID_NDK_HOME/ANDROID_NDK_ROOT, keeping SDK paths out of the repository.
if(DEFINED CMAKE_ANDROID_NDK)
    set(_mira_android_ndk "${CMAKE_ANDROID_NDK}")
elseif(DEFINED ENV{ANDROID_NDK_HOME})
    set(_mira_android_ndk "$ENV{ANDROID_NDK_HOME}")
elseif(DEFINED ENV{ANDROID_NDK_ROOT})
    set(_mira_android_ndk "$ENV{ANDROID_NDK_ROOT}")
else()
    message(FATAL_ERROR
        "Mira Android builds require CMAKE_ANDROID_NDK, ANDROID_NDK_HOME, or ANDROID_NDK_ROOT")
endif()

if(NOT EXISTS "${_mira_android_ndk}/build/cmake/android.toolchain.cmake")
    message(FATAL_ERROR "Android NDK toolchain not found under ${_mira_android_ndk}")
endif()

# These are the public inputs consumed by the NDK toolchain. The
# CMAKE_ANDROID_* cache variables are derived from them while it initializes.
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 24 CACHE STRING "Minimum Android API level" FORCE)
set(ANDROID_PLATFORM android-24 CACHE STRING "Minimum Android API level" FORCE)
set(ANDROID_ABI arm64-v8a CACHE STRING "Android ABI" FORCE)
set(ANDROID_STL c++_static CACHE STRING "Android STL" FORCE)
set(CMAKE_ANDROID_NDK "${_mira_android_ndk}" CACHE PATH "Android NDK" FORCE)

include("${CMAKE_ANDROID_NDK}/build/cmake/android.toolchain.cmake")

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "Mira Android arm64 toolchain resolved unexpected ABI: ${CMAKE_ANDROID_ARCH_ABI}")
endif()
if(NOT CMAKE_SYSTEM_VERSION VERSION_EQUAL "24")
    message(FATAL_ERROR
        "Mira Android arm64 toolchain resolved unexpected API level: ${CMAKE_SYSTEM_VERSION}")
endif()
