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

# These are the inputs consumed by CMake's built-in Android platform support.
# Do not include the NDK's legacy wrapper here: CMake loads its Android modules
# after this file and nested inclusion makes the ABI/API selection order-dependent.
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 24 CACHE STRING "Minimum Android API level" FORCE)
set(CMAKE_ANDROID_API 24 CACHE STRING "Minimum Android API level" FORCE)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a CACHE STRING "Android ABI" FORCE)
set(CMAKE_ANDROID_STL_TYPE c++_static CACHE STRING "Android STL" FORCE)
set(CMAKE_ANDROID_NDK "${_mira_android_ndk}" CACHE PATH "Android NDK" FORCE)
