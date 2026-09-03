function(mira_add_test name source label)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${label}" TIMEOUT 90)
endfunction()

mira_add_test(mira_unit_test unit/baseline_support_test.cpp unit)
mira_add_test(mira_contract_test contract/environment_contract_test.cpp contract)
mira_add_test(mira_executor_integration_test integration/executor_lifecycle_test.cpp integration)
mira_add_test(mira_replay_test replay/deterministic_replay_test.cpp replay)
mira_add_test(mira_stress_test stress/runtime_ordering_stress_test.cpp stress)
mira_add_test(mira_benchmark_smoke benchmark/control_plane_benchmark.cpp benchmark)
mira_add_test(mira_m1_core_test m1/m1_core_test.cpp contract)
mira_add_test(mira_m2_observation_test m2/m2_observation_coordinate_test.cpp contract)
mira_add_test(mira_m2_simulator_test m2/m2_simulator_scenario_test.cpp contract)
mira_add_test(mira_m2_pipeline_test m2/m2_observation_pipeline_test.cpp contract)

# ---------------------------------------------------------------------------
# M3: model provider and agent loop
# ---------------------------------------------------------------------------

function(mira_add_m3_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::simulator_adapter Mira::net_transport executor::executor)
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/adapters/net")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "contract;m3" TIMEOUT 120)
endfunction()

mira_add_m3_test(mira_m3_canonical_test m3/m3_canonical_test.cpp)
mira_add_m3_test(mira_m3_schema_test m3/m3_schema_test.cpp)
mira_add_m3_test(mira_m3_dialect_test m3/m3_dialect_test.cpp)
mira_add_m3_test(mira_m3_sse_test m3/m3_sse_test.cpp)
mira_add_m3_test(mira_m3_tool_test m3/m3_tool_test.cpp)
mira_add_m3_test(mira_m3_supervisor_budget_test m3/m3_supervisor_budget_test.cpp)
mira_add_m3_test(mira_m3_transport_test m3/m3_transport_test.cpp)
mira_add_m3_test(mira_m3_gateway_test m3/m3_gateway_test.cpp)
mira_add_m3_test(mira_m3_agent_loop_test m3/m3_agent_loop_test.cpp)
mira_add_m3_test(mira_m3_replay_test m3/m3_replay_test.cpp)
mira_add_m3_test(mira_m3_upload_test m3/m3_upload_test.cpp)

if(TARGET mira_openssl_transport)
    mira_add_m3_test(mira_m3_tls_test m3/m3_tls_test.cpp)
    target_link_libraries(mira_m3_tls_test PRIVATE Mira::openssl_transport)
endif()

if(TARGET mira_mbedtls_transport AND OpenSSL_FOUND)
    mira_add_m3_test(mira_m3_mbedtls_test m3/m3_tls_test.cpp)
    target_compile_definitions(mira_m3_mbedtls_test PRIVATE MIRA_TEST_MBEDTLS=1)
    target_link_libraries(mira_m3_mbedtls_test PRIVATE Mira::mbedtls_transport OpenSSL::SSL)
endif()

if(TARGET mira_mbedtls_transport)
    mira_add_m3_test(mira_m3_mbedtls_portable_test m3/m3_mbedtls_portable_test.cpp)
    target_include_directories(mira_m3_mbedtls_portable_test SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/mbedtls/include"
        "${PROJECT_SOURCE_DIR}/third_party/mbedtls/tests/include")
    target_link_libraries(mira_m3_mbedtls_portable_test PRIVATE
        Mira::mbedtls_transport MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    if(WIN32)
        set_tests_properties(mira_m3_mbedtls_portable_test PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:mira_mbedtls_transport>")
    endif()
    if(MIRA_ENABLE_TSAN)
        # The portable fixture runs a Mbed TLS server and client in one process.
        # Their shared upstream PSA global state is not a Mira concurrency path;
        # TSAN still covers the client adapter against the OpenSSL server fixture.
        set_tests_properties(mira_m3_mbedtls_portable_test PROPERTIES DISABLED TRUE)
    endif()
endif()

if(TARGET mira_mbedtls_transport)
    add_executable(mira_m3_interop_probe m3/m3_interop_probe.cpp)
    target_link_libraries(mira_m3_interop_probe PRIVATE
        Mira::core Mira::net_transport Mira::mbedtls_transport executor::executor)
    target_include_directories(mira_m3_interop_probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/adapters/net")
    target_include_directories(mira_m3_interop_probe SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(mira_m3_interop_probe)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(mira_m3_interop_probe PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(mira_m3_interop_probe)
endif()

# ---------------------------------------------------------------------------
# M4: context, memory and recovery foundations
# ---------------------------------------------------------------------------

function(mira_add_m4_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "contract;m4" TIMEOUT 120)
endfunction()

mira_add_m4_test(mira_m4_contracts_test m4/m4_contracts_test.cpp)
mira_add_m4_test(mira_m4_context_manager_test m4/m4_context_manager_test.cpp)
mira_add_m4_test(mira_m4_checkpoint_test m4/m4_checkpoint_test.cpp)
mira_add_m4_test(mira_m4_recovery_test m4/m4_recovery_test.cpp)

# Durable SQLite/WAL reference backends need mira_state_store.
function(mira_add_m4_store_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::state_store executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "contract;m4" TIMEOUT 180)
endfunction()

mira_add_m4_store_test(mira_m4_sqlite_checkpoint_test m4/m4_sqlite_checkpoint_test.cpp)
mira_add_m4_store_test(mira_m4_sqlite_memory_test m4/m4_sqlite_memory_test.cpp)
mira_add_m4_store_test(mira_m4_memory_retrieval_test m4/m4_memory_retrieval_test.cpp)
mira_add_m4_store_test(mira_m4_consolidation_test m4/m4_consolidation_test.cpp)
mira_add_m4_store_test(mira_m4_stateful_replay_test m4/m4_stateful_replay_test.cpp)
mira_add_m4_test(mira_m4_continuation_test m4/m4_continuation_test.cpp)
mira_add_m4_test(mira_m4_supervisor_test m4/m4_supervisor_test.cpp)

# M4-17: long-task and memory benchmark harness. Asserts fidelity invariants;
# writes a JSON manifest for docs/benchmarks/long-task-memory.md.
add_executable(mira_m4_stateful_benchmark benchmark/m4_stateful_benchmark.cpp)
target_link_libraries(mira_m4_stateful_benchmark PRIVATE
    Mira::core Mira::state_store executor::executor)
target_include_directories(mira_m4_stateful_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(mira_m4_stateful_benchmark SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/executor/include")
mira_enable_warnings(mira_m4_stateful_benchmark)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mira_m4_stateful_benchmark PRIVATE -Wno-deprecated-declarations)
endif()
mira_enable_sanitizers(mira_m4_stateful_benchmark)
add_test(NAME mira_m4_stateful_benchmark COMMAND mira_m4_stateful_benchmark)
set_tests_properties(mira_m4_stateful_benchmark PROPERTIES LABELS "benchmark;m4" TIMEOUT 300)

add_executable(mira_m2_android_host_test
    m2/m2_android_host_abi_test.cpp
    support/fake_android_host.cpp
)
target_link_libraries(mira_m2_android_host_test PRIVATE
    Mira::core Mira::android_adapter Mira::simulator_adapter executor::executor)
target_include_directories(mira_m2_android_host_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(mira_m2_android_host_test SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/executor/include")
mira_enable_warnings(mira_m2_android_host_test)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mira_m2_android_host_test PRIVATE -Wno-deprecated-declarations)
endif()
mira_enable_sanitizers(mira_m2_android_host_test)
add_test(NAME mira_m2_android_host_test COMMAND mira_m2_android_host_test)
set_tests_properties(mira_m2_android_host_test PROPERTIES LABELS "contract;m2" TIMEOUT 90)

add_test(
    NAME mira_public_headers_test
    COMMAND "${CMAKE_COMMAND}"
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DBINARY_DIR=${PROJECT_BINARY_DIR}/header-check
            -DGENERATOR=${CMAKE_GENERATOR}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunHeaderCheck.cmake
)
set_tests_properties(mira_public_headers_test PROPERTIES LABELS "contract;headers" TIMEOUT 90)

add_test(
    NAME mira_installed_consumer_test
    COMMAND "${CMAKE_COMMAND}"
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DBINARY_DIR=${PROJECT_BINARY_DIR}/consumer-check
            -DPROJECT_BUILD_DIR=${PROJECT_BINARY_DIR}
            -DGENERATOR=${CMAKE_GENERATOR}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
            -DBUILD_CONFIG=$<CONFIG>
            -DMIRA_ENABLE_ASAN=${MIRA_ENABLE_ASAN}
            -DMIRA_ENABLE_UBSAN=${MIRA_ENABLE_UBSAN}
            -DMIRA_ENABLE_TSAN=${MIRA_ENABLE_TSAN}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunInstalledConsumer.cmake
)
set_tests_properties(mira_installed_consumer_test PROPERTIES LABELS "contract;consumer" TIMEOUT 180)

if(Python3_Interpreter_FOUND)
    add_test(NAME mira_docs_test
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_docs.py"
                "${PROJECT_SOURCE_DIR}")
    set_tests_properties(mira_docs_test PROPERTIES LABELS "docs")
    add_test(NAME mira_sbom_test
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_sbom.py"
                "${PROJECT_SOURCE_DIR}")
    set_tests_properties(mira_sbom_test PROPERTIES LABELS "supply-chain")
    add_test(NAME mira_platform_boundary_test
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_platform_boundary.py"
                "${PROJECT_SOURCE_DIR}")
    set_tests_properties(mira_platform_boundary_test PROPERTIES LABELS "contract;platform")
endif()
