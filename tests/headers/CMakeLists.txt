cmake_minimum_required(VERSION 3.20)
project(MiraHeaderCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

foreach(HEADER IN ITEMS
        mira/version.hpp
        mira/environment.hpp
        mira/runtime_baseline.hpp
        mira/core_contracts.hpp
        mira/coordinates.hpp
        mira/observation.hpp
        mira/observation_pipeline.hpp
        mira/event_store.hpp
        mira/action_journal.hpp
        mira/artifact_store.hpp
        mira/security.hpp
        mira/replay.hpp
        mira/runtime.hpp
        mira/adapters/simulator/simulator_environment.hpp
        mira/adapters/android/host_dispatcher.hpp
        mira/adapters/android/android_host_adapter.hpp
        mira/adapters/net/mbedtls_tls.hpp
        mira/adapters/net/openssl_tls.hpp
        mira/adapters/net/socket_transport.hpp
        mira/workflow_ir.hpp
        mira/workflow_run.hpp
        mira/workflow_versioning.hpp
        mira/workflow_events.hpp
        mira/workflow_tools.hpp
        mira/workflow_compiler.hpp
        mira/workflow_runtime.hpp)
    string(REPLACE "/" "_" TARGET_NAME "${HEADER}")
    string(REPLACE "." "_" TARGET_NAME "${TARGET_NAME}")
    set(SOURCE_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.cpp")
    file(WRITE "${SOURCE_FILE}" "#include <${HEADER}>\nint main() { return 0; }\n")
    add_executable("header_${TARGET_NAME}" "${SOURCE_FILE}")
    target_include_directories("header_${TARGET_NAME}" PRIVATE "${MIRA_INCLUDE_DIR}")
    # The net transport headers reference Executor worker types in their
    # public surface, so their minimal consumer needs the Executor headers
    # exactly like an installed Mira::net_transport consumer does.
    if(HEADER MATCHES "adapters/net/" AND EXECUTOR_INCLUDE_DIR)
        target_include_directories("header_${TARGET_NAME}" SYSTEM PRIVATE
            "${EXECUTOR_INCLUDE_DIR}")
    endif()
endforeach()
