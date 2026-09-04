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
option(MIRA_WITH_OPENSSL "Build the OpenSSL-backed TLS channel adapter" ON)
option(MIRA_WITH_MBEDTLS "Build the pinned cross-platform Mbed TLS channel adapter" ON)

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

if(MIRA_WITH_MBEDTLS)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)
    set_target_properties(mbedcrypto mbedx509 mbedtls everest p256m
        PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Pinned SQLite amalgamation backing the durable state stores (M4-06/M4-09).
add_subdirectory(third_party/sqlite)

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
    src/runtime/task_recovery.cpp
    src/runtime/context_memory_supervisor.cpp
    src/context/context_contracts.cpp
    src/context/context_manager.cpp
    src/context/memory_contracts.cpp
    src/context/memory_consolidation.cpp
    src/observation/coordinate_transform.cpp
    src/observation/observation.cpp
    src/observation/observation_pipeline.cpp
    src/storage/event_store.cpp
    src/storage/artifact_store.cpp
    src/storage/action_journal.cpp
    src/storage/checkpoint_store.cpp
    src/security/security.cpp
    src/replay/replay.cpp
    src/replay/stateful_replay.cpp
    src/support/json.cpp
    src/model/model_contracts.cpp
    src/model/model_profile.cpp
    src/model/model_digest.cpp
    src/model/model_schema.cpp
    src/model/model_dialect.cpp
    src/model/model_sse.cpp
    src/model/model_tool.cpp
    src/model/provider_continuation.cpp
    src/model/model_budget.cpp
    src/model/model_supervisor.cpp
    src/model/model_provider.cpp
    src/model/model_upload.cpp
    src/model/model_gateway.cpp
    src/model/model_replay.cpp
    src/model/agent_loop.cpp
)
add_library(Mira::core ALIAS mira_core)

# Durable SQLite/WAL reference stores for checkpoints and long-term memory.
# Kept outside mira_core so the installed Core contract stays SQLite-free;
# public headers in include/mira never expose sqlite3 types.
add_library(mira_state_store
    src/storage/sqlite_support.cpp
    src/storage/sqlite_checkpoint_store.cpp
    src/storage/sqlite_memory_store.cpp
)
add_library(Mira::state_store ALIAS mira_state_store)
target_include_directories(mira_state_store
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(mira_state_store
    PUBLIC Mira::core
    PRIVATE Mira::sqlite3 executor::executor
)
target_include_directories(mira_state_store SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/executor/include"
)
target_compile_features(mira_state_store PUBLIC cxx_std_20)
mira_enable_warnings(mira_state_store)
if(MIRA_ENABLE_CLANG_TIDY)
    set_property(TARGET mira_state_store PROPERTY CXX_CLANG_TIDY
        "${MIRA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mira_state_store PRIVATE -Wno-deprecated-declarations)
endif()
mira_enable_sanitizers(mira_state_store)
set_target_properties(mira_state_store PROPERTIES EXPORT_NAME state_store)
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

add_library(mira_android_adapter
    adapters/android/host_dispatcher.cpp
    adapters/android/android_host_adapter.cpp
)
add_library(Mira::android_adapter ALIAS mira_android_adapter)
target_include_directories(mira_android_adapter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(mira_android_adapter PUBLIC Mira::core)
target_link_libraries(mira_android_adapter PRIVATE executor::executor)
target_include_directories(mira_android_adapter SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/executor/include"
)
target_compile_features(mira_android_adapter PUBLIC cxx_std_20)
mira_enable_warnings(mira_android_adapter)
if(MIRA_ENABLE_CLANG_TIDY)
    set_property(TARGET mira_android_adapter PROPERTY CXX_CLANG_TIDY
        "${MIRA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
    )
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mira_android_adapter PRIVATE -Wno-deprecated-declarations)
endif()
mira_enable_sanitizers(mira_android_adapter)
set_target_properties(mira_android_adapter PROPERTIES EXPORT_NAME android_adapter)

# Portable socket HTTP/SSE transport adapter (POSIX sockets + Winsock).
add_library(mira_net_transport
    adapters/net/socket_transport.cpp
)
add_library(Mira::net_transport ALIAS mira_net_transport)
target_include_directories(mira_net_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/adapters/net
)
target_link_libraries(mira_net_transport
    PUBLIC Mira::core
    PRIVATE executor::executor
)
target_include_directories(mira_net_transport SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/executor/include"
)
target_compile_features(mira_net_transport PUBLIC cxx_std_20)
mira_enable_warnings(mira_net_transport)
if(MIRA_ENABLE_CLANG_TIDY)
    set_property(TARGET mira_net_transport PROPERTY CXX_CLANG_TIDY
        "${MIRA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
    )
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mira_net_transport PRIVATE -Wno-deprecated-declarations)
endif()
mira_enable_sanitizers(mira_net_transport)
set_target_properties(mira_net_transport PROPERTIES EXPORT_NAME net_transport)
if(WIN32)
    target_link_libraries(mira_net_transport PRIVATE ws2_32)
endif()

# Optional OpenSSL-backed TLS channel for https endpoints. Built only where
# OpenSSL development files exist and the platform combination is verified;
# everywhere else https fails closed at the transport boundary.
if(MIRA_WITH_OPENSSL AND UNIX AND NOT ANDROID)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        add_library(mira_openssl_transport
            adapters/net/openssl_tls.cpp
        )
        add_library(Mira::openssl_transport ALIAS mira_openssl_transport)
        target_include_directories(mira_openssl_transport
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/net
        )
        target_link_libraries(mira_openssl_transport
            PUBLIC Mira::core
            PRIVATE OpenSSL::SSL executor::executor
        )
        target_include_directories(mira_openssl_transport SYSTEM PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/executor/include"
        )
        target_compile_features(mira_openssl_transport PUBLIC cxx_std_20)
        mira_enable_warnings(mira_openssl_transport)
        if(MIRA_ENABLE_CLANG_TIDY)
            set_property(TARGET mira_openssl_transport PROPERTY CXX_CLANG_TIDY
                "${MIRA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
            )
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(mira_openssl_transport PRIVATE -Wno-deprecated-declarations)
        endif()
        mira_enable_sanitizers(mira_openssl_transport)
        set_target_properties(mira_openssl_transport PROPERTIES EXPORT_NAME openssl_transport)
        message(STATUS "Mira: OpenSSL ${OPENSSL_VERSION} found; building the TLS channel adapter")
    else()
        message(STATUS "Mira: OpenSSL not found; https endpoints will fail closed")
    endif()
endif()

# Pinned cross-platform TLS channel. Mbed TLS owns no worker threads here: its
# nonblocking BIO callbacks operate on SocketHttpTransport's native socket and
# all waits remain on the Executor blocking-I/O worker.
if(MIRA_WITH_MBEDTLS)
    add_library(mira_mbedtls_transport SHARED
        adapters/net/mbedtls_tls.cpp
    )
    add_library(Mira::mbedtls_transport ALIAS mira_mbedtls_transport)
    target_include_directories(mira_mbedtls_transport
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/adapters/net
    )
    # Mbed TLS exposes C-style compatibility macros in public headers. Treat
    # the pinned dependency as a system include so Mira's C++ warning policy
    # does not turn upstream -Wold-style-cast diagnostics into build errors.
    target_include_directories(mira_mbedtls_transport SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
    )
    target_link_libraries(mira_mbedtls_transport
        PUBLIC Mira::core
        PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto
    )
    target_compile_features(mira_mbedtls_transport PUBLIC cxx_std_20)
    mira_enable_warnings(mira_mbedtls_transport)
    if(MIRA_ENABLE_CLANG_TIDY)
        set_property(TARGET mira_mbedtls_transport PROPERTY CXX_CLANG_TIDY
            "${MIRA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
        )
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(mira_mbedtls_transport PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(mira_mbedtls_transport)
    set_target_properties(mira_mbedtls_transport PROPERTIES
        EXPORT_NAME mbedtls_transport
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

add_executable(mira_minimal_consumer examples/minimal_consumer.cpp)
target_link_libraries(mira_minimal_consumer PRIVATE Mira::core Mira::simulator_adapter)
mira_enable_warnings(mira_minimal_consumer)
mira_enable_sanitizers(mira_minimal_consumer)

# Stateful agent beta example (M4-18): durable stores are in-tree targets.
add_executable(mira_stateful_consumer examples/stateful_agent_consumer.cpp)
target_link_libraries(mira_stateful_consumer PRIVATE
    Mira::core Mira::state_store executor::executor)
target_include_directories(mira_stateful_consumer SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/executor/include")
mira_enable_warnings(mira_stateful_consumer)
mira_enable_sanitizers(mira_stateful_consumer)
if(MIRA_BUILD_TESTS)
    add_test(NAME mira_stateful_consumer_test COMMAND mira_stateful_consumer)
    set_tests_properties(mira_stateful_consumer_test PROPERTIES LABELS "consumer" TIMEOUT 60)
endif()

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

install(TARGETS mira_core mira_state_store mira_simulator_adapter mira_android_adapter mira_net_transport
    EXPORT MiraTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
# The vendored SQLite archive backs Mira::state_store; exporting it keeps the
# installed package self-contained ($<LINK_ONLY:...> closures resolve within
# the same export set). Threads::Threads from its PRIVATE deps needs the
# find_dependency(Threads) entry in MiraConfig.cmake.in.
install(TARGETS sqlite3
    EXPORT MiraTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
if(TARGET mira_openssl_transport)
    install(TARGETS mira_openssl_transport
        EXPORT MiraTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
if(TARGET mira_mbedtls_transport)
    install(TARGETS mira_mbedtls_transport
        EXPORT MiraTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
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
