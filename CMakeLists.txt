cmake_minimum_required(VERSION 3.20)

project(Mira VERSION 0.1.0 LANGUAGES CXX)

if(ANDROID)
    if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR
            "Mira Android preset resolved unexpected ABI: ${CMAKE_ANDROID_ARCH_ABI}")
    endif()
    if(NOT CMAKE_SYSTEM_VERSION VERSION_EQUAL "24")
        message(FATAL_ERROR
            "Mira Android preset resolved unexpected API level: ${CMAKE_SYSTEM_VERSION}")
    endif()
endif()

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

option(MIRA_BUILD_TESTS "Build Mira tests" ${BUILD_TESTING})
option(MIRA_WARNINGS_AS_ERRORS "Treat Mira warnings as errors" ON)
option(MIRA_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MIRA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MIRA_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MIRA_ENABLE_CLANG_TIDY "Run clang-tidy while compiling Mira targets" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MiraWarnings)
include(MiraSanitizers)
include(MiraPlatform)

mira_report_platform()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MIRA_ENABLE_CLANG_TIDY)
    find_program(MIRA_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
endif()

# Executor is pinned as a git submodule. Mira owns its lifecycle but does not
# modify its build defaults globally or build its upstream tests here.
set(EXECUTOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXECUTOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXECUTOR_ENABLE_GPU OFF CACHE BOOL "" FORCE)
set(EXECUTOR_ENABLE_CUDA OFF CACHE BOOL "" FORCE)
set(EXECUTOR_ENABLE_OPENCL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/executor)

if(MIRA_ENABLE_ASAN)
    target_compile_options(executor PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(executor PRIVATE -fsanitize=address)
endif()
if(MIRA_ENABLE_UBSAN)
    target_compile_options(executor PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(executor PRIVATE -fsanitize=undefined)
endif()
if(MIRA_ENABLE_TSAN)
    target_compile_options(executor PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(executor PRIVATE -fsanitize=thread)
endif()

add_library(mira_core
    src/runtime/runtime_baseline.cpp
    src/runtime/runtime.cpp
    src/observation/coordinate_transform.cpp
    src/observation/observation.cpp
    src/storage/event_store.cpp
    src/storage/artifact_store.cpp
    src/storage/action_journal.cpp
    src/security/security.cpp
    src/replay/replay.cpp
)
add_library(Mira::core ALIAS mira_core)
target_include_directories(mira_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(mira_core PRIVATE executor::executor)
target_include_directories(mira_core SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/executor/include"
)
target_compile_features(mira_core PUBLIC cxx_std_20)
mira_enable_warnings(mira_core)
if(MIRA_ENABLE_CLANG_TIDY)
    set_property(TARGET mira_core PROPERTY CXX_CLANG_TIDY
        "${MIRA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mira_core PRIVATE -Wno-deprecated-declarations)
endif()
mira_enable_sanitizers(mira_core)
set_target_properties(mira_core PROPERTIES
    EXPORT_NAME core
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_library(mira_simulator_adapter
    adapters/simulator/simulator_environment.cpp
)
add_library(Mira::simulator_adapter ALIAS mira_simulator_adapter)
target_include_directories(mira_simulator_adapter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(mira_simulator_adapter PUBLIC Mira::core)
target_compile_features(mira_simulator_adapter PUBLIC cxx_std_20)
mira_enable_warnings(mira_simulator_adapter)
if(MIRA_ENABLE_CLANG_TIDY)
    set_property(TARGET mira_simulator_adapter PROPERTY CXX_CLANG_TIDY
        "${MIRA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mira_simulator_adapter PRIVATE -Wno-deprecated-declarations)
endif()
mira_enable_sanitizers(mira_simulator_adapter)
set_target_properties(mira_simulator_adapter PROPERTIES EXPORT_NAME simulator_adapter)

add_executable(mira_minimal_consumer examples/minimal_consumer.cpp)
target_link_libraries(mira_minimal_consumer PRIVATE Mira::core Mira::simulator_adapter)
mira_enable_warnings(mira_minimal_consumer)
mira_enable_sanitizers(mira_minimal_consumer)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(docs-check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_docs.py"
                "${CMAKE_CURRENT_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking Markdown links and fences"
        VERBATIM
    )
    add_custom_target(sbom-check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_sbom.py"
                "${CMAKE_CURRENT_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking pinned dependencies and SBOM"
        VERBATIM
    )
    add_custom_target(platform-boundary-check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_platform_boundary.py"
                "${CMAKE_CURRENT_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking Mira Core platform boundary"
        VERBATIM
    )
endif()

add_custom_target(format-check
    COMMAND "${CMAKE_COMMAND}"
            -DROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckFormat.cmake"
    COMMENT "Checking C++ formatting"
    VERBATIM
)

if(MIRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS mira_core mira_simulator_adapter
    EXPORT MiraTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MiraTargets
    FILE MiraTargets.cmake
    NAMESPACE Mira::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Mira
)
configure_package_config_file(
    cmake/MiraConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/MiraConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Mira
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MiraConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MiraConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MiraConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Mira
)
