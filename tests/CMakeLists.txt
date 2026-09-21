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
        "${CMAKE_CURRENT_SOURCE_DIR}")
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
mira_add_m3_test(mira_m3_tool_executor_test m3/m3_tool_executor_test.cpp)
mira_add_m3_test(mira_m3_tool_loop_test m3/m3_tool_loop_test.cpp)
mira_add_m3_test(mira_m3_replay_test m3/m3_replay_test.cpp)
mira_add_m3_test(mira_m3_upload_test m3/m3_upload_test.cpp)
mira_add_test(mira_agent_harness_test integration/agent_harness_test.cpp integration)

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
        "${CMAKE_CURRENT_SOURCE_DIR}")
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
    set_tests_properties(${name} PROPERTIES LABELS "contract;m4" TIMEOUT 300)
endfunction()

mira_add_m4_store_test(mira_m4_sqlite_checkpoint_test m4/m4_sqlite_checkpoint_test.cpp)
mira_add_m4_store_test(mira_m4_sqlite_memory_test m4/m4_sqlite_memory_test.cpp)
mira_add_m4_store_test(mira_m4_memory_retrieval_test m4/m4_memory_retrieval_test.cpp)
mira_add_m4_store_test(mira_m4_consolidation_test m4/m4_consolidation_test.cpp)
mira_add_m4_store_test(mira_m4_stateful_replay_test m4/m4_stateful_replay_test.cpp)
mira_add_m4_test(mira_m4_continuation_test m4/m4_continuation_test.cpp)
mira_add_m4_test(mira_m4_supervisor_test m4/m4_supervisor_test.cpp)

# ---------------------------------------------------------------------------
# M8: workflow dual-path contracts (pure functions; no Executor routing)
# ---------------------------------------------------------------------------

function(mira_add_m8_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::workflow)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    mira_enable_warnings(${name})
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "contract;m8" TIMEOUT 90)
endfunction()

mira_add_m8_test(mira_m8_ir_test m8/m8_ir_test.cpp)
mira_add_m8_test(mira_m8_run_test m8/m8_run_test.cpp)
mira_add_m8_test(mira_m8_versioning_test m8/m8_versioning_test.cpp)
mira_add_m8_test(mira_m8_events_test m8/m8_events_test.cpp)
mira_add_m8_test(mira_m8_tools_test m8/m8_tools_test.cpp)

# ---------------------------------------------------------------------------
# M9: workflow runtime minimal loop (stage B; Executor-routed execution)
# ---------------------------------------------------------------------------

function(mira_add_m9_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m9" TIMEOUT 120)
endfunction()

mira_add_m9_test(mira_m9_execution_test m9/m9_execution_test.cpp)
mira_add_m9_test(mira_m9_control_test m9/m9_control_test.cpp)
mira_add_m9_test(mira_m9_tools_test m9/m9_tools_test.cpp)

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
set_tests_properties(mira_m4_stateful_benchmark PROPERTIES LABELS "benchmark;m4" TIMEOUT 900)

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

# ---------------------------------------------------------------------------
# M10: workflow intervention and the full policy set (stage C)
# ---------------------------------------------------------------------------

function(mira_add_m10_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m10" TIMEOUT 120)
endfunction()

mira_add_m10_test(mira_m10_policy_test m10/m10_policy_test.cpp)
mira_add_m10_test(mira_m10_patch_test m10/m10_patch_test.cpp)
mira_add_m10_test(mira_m10_interaction_test m10/m10_interaction_test.cpp)

# ---------------------------------------------------------------------------
# M11: trajectory compilation, task induction and the publish gate (stage D)
# ---------------------------------------------------------------------------

function(mira_add_m11_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m11" TIMEOUT 120)
endfunction()

mira_add_m11_test(mira_m11_compile_test m11/m11_compile_test.cpp)
mira_add_m11_test(mira_m11_publish_test m11/m11_publish_test.cpp)
mira_add_m11_test(mira_m11_induction_test m11/m11_induction_test.cpp)

# ---------------------------------------------------------------------------
# M12: stage E - App Model and navigation
# ---------------------------------------------------------------------------

function(mira_add_m12_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m12" TIMEOUT 120)
endfunction()

mira_add_m12_test(mira_m12_contract_test m12/m12_contract_test.cpp)
mira_add_m12_test(mira_m12_navigation_test m12/m12_navigation_test.cpp)
mira_add_m12_test(mira_m12_events_test m12/m12_events_test.cpp)

# ---------------------------------------------------------------------------
# M13: stage F - memory domains and the learning loop
# ---------------------------------------------------------------------------

function(mira_add_m13_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m13" TIMEOUT 120)
endfunction()

mira_add_m13_test(mira_m13_contract_test m13/m13_contract_test.cpp)
mira_add_m13_test(mira_m13_learning_test m13/m13_learning_test.cpp)
mira_add_m13_test(mira_m13_events_test m13/m13_events_test.cpp)

# MNT-202609-25: learning persistence against the real SQLite backend and the
# DEC-030 §5 event-rebuild recipe need Mira::state_store as well.
function(mira_add_m13_persistence_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter Mira::state_store
        executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m13" TIMEOUT 300)
endfunction()

mira_add_m13_persistence_test(mira_m13_learning_persistence_test
    m13/m13_learning_persistence_test.cpp)

# M14 (DEC-031): the recovery orchestrator lives in Mira::workflow; its tests
# also assemble a ModelGateway over a scripted provider.
function(mira_add_m14_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m14" TIMEOUT 180)
endfunction()

mira_add_m14_test(mira_m14_recovery_test m14/m14_recovery_test.cpp)

# M15 (MNT-202609-29, DEC-034): the minimal eval profile harness. The
# recorded round doubles as a regression gate: any G1-G6 or budget failure
# exits non-zero. Links the state store for the D-arm SQLite learning
# context (same shape as the M13 persistence test).
function(mira_add_m15_eval name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter Mira::state_store
        executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m15" TIMEOUT 900)
endfunction()

mira_add_m15_eval(mira_m15_eval_profile_harness m15/m15_eval_profile_harness.cpp)

# M16 (DEC-032 Stage A): the long-session Layer 0 baseline harness. A pure
# deterministic single-threaded driver over StandardContextManager; any
# pre-frozen G1-G6 gate failure exits non-zero so CI treats the baseline as
# a regression gate. No model, no async work, no executor tasks.
function(mira_add_m16_baseline name source)
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
    set_tests_properties(${name} PROPERTIES LABELS "integration;m16" TIMEOUT 600)
endfunction()

mira_add_m16_baseline(mira_m16_long_session_baseline m16/m16_long_session_baseline.cpp)

# M17 (DEC-032 Stage B): Layer 1 retrieval contracts and the in-memory
# reference index; the contract suite also exercises the supervisor routing
# (shutdown rejection) so it links the executor. The eval harness runs the
# frozen retrieval dataset with R1-R4 gates and exits non-zero on failure.
function(mira_add_m17_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::workflow executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m17" TIMEOUT 120)
endfunction()

mira_add_m17_test(mira_m17_context_retrieval_test m17/m17_context_retrieval_test.cpp)
mira_add_m17_test(mira_m17_retrieval_eval m17/m17_retrieval_eval.cpp)

# M18 (DEC-032 Stage C): Layer 2 rerank contract and the deterministic
# reference reranker; the contract suite also exercises the supervisor routing
# (shutdown rejection). The eval harness re-runs the frozen M17 dataset and
# gates the B-vs-C comparison (C1-C4), exiting non-zero on failure.
function(mira_add_m18_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::workflow executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m18" TIMEOUT 120)
endfunction()

mira_add_m18_test(mira_m18_context_rerank_test m18/m18_context_rerank_test.cpp)
mira_add_m18_test(mira_m18_rerank_eval m18/m18_rerank_eval.cpp)


# M19 (DEC-032 Stage D): Layer 3 semantic consolidation — checkpoint
# contracts, the model-backed reference consolidator, the commit pipeline
# (five-tuple validation, terminal idempotency) and the supervisor Deferrable
# routing. The eval harness runs the frozen consolidation dataset with gates
# D1-D5 and exits non-zero on failure.
function(mira_add_m19_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::workflow executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m19" TIMEOUT 120)
endfunction()

mira_add_m19_test(mira_m19_context_consolidation_test m19/m19_context_consolidation_test.cpp)
mira_add_m19_test(mira_m19_consolidation_eval m19/m19_consolidation_eval.cpp)

# M20 (DEC-035 Stage W1): WorkingContextSnapshot — deterministic projection
# from committed checkpoints, the monotonic commit pipeline (five-tuple
# validation, terminal idempotency, same-watermark conflict fail-closed) and
# the supervisor Deferrable routing. The eval harness runs the frozen
# checkpoint-chain dataset with gates W1-G1..G6 and exits non-zero on failure.
function(mira_add_m20_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::workflow executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m20" TIMEOUT 120)
endfunction()

mira_add_m20_test(mira_m20_working_context_test m20/m20_working_context_test.cpp)
mira_add_m20_test(mira_m20_working_context_eval m20/m20_working_context_eval.cpp)

# M21 (DEC-035 Stage W2): the model-mediated IContextCurator — previous
# snapshot + checkpoint + recent events curation with provenance binding and
# the degenerate-merge guard, the schema 1.1 snapshot extension (five
# Curator sections, generated_by) and the supervisor Deferrable routing. The
# eval harness runs the frozen curation dataset with gates W2-G1..G6 and
# exits non-zero on failure.
function(mira_add_m21_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::workflow executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m21" TIMEOUT 120)
endfunction()

mira_add_m21_test(mira_m21_context_curator_test m21/m21_context_curator_test.cpp)
mira_add_m21_test(mira_m21_curator_eval m21/m21_curator_eval.cpp)

# M22 (DEC-035 Stage W3): the supervisor-driven automatic Working Context
# refresh — trigger policy (watermark / event-count axes), per-session-chain
# coalescing, the task-boundary forced flush and the failure fallback, all
# routed through the existing supervisor Deferrable curation route. The eval
# harness runs the frozen auto-trigger dataset with gates W3-G1..G6 and exits
# non-zero on failure.
function(mira_add_m22_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Mira::core Mira::workflow executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "integration;m22" TIMEOUT 120)
endfunction()

mira_add_m22_test(mira_m22_auto_trigger_test m22/m22_auto_trigger_test.cpp)
mira_add_m22_test(mira_m22_auto_trigger_eval m22/m22_auto_trigger_eval.cpp)

# ---------------------------------------------------------------------------
# M7 (DEC-009 Stage TM0): the tool module contract layer - governed capability
# catalog, fail-closed manifest parsing and deterministic negotiation. Pure
# computation over caller-supplied data; gates M7-TM0-G1..G5 live in the test,
# G6 in examples/minimal_consumer.cpp.
# TM1 (registry lifecycle, DEC-009/DEC-042): origin trust, state machine,
# tombstone, negotiation trigger hooks and the Executor-routed deploy
# verification path; gates M7-TM1-G1..G6.
# MCP admission stage (DEC-039): listing -> out-of-process module conversion,
# session lifecycle downgrade-only mapping, DEC-015-consistent dispatcher and
# cancellation/deadline/shutdown closure; gates M7-MCP-G1..G6.
# TR0 stable references (DEC-040): reference syntax, refs-manifest extraction,
# resolution matrix and the Runnable/Degraded/Invalid compatibility projection;
# gates M7-TR0-G1..G6.
# ---------------------------------------------------------------------------

mira_add_test(mira_m7_tool_module_test m7/m7_tool_module_test.cpp contract)
mira_add_test(mira_m7_tool_module_registry_test m7/m7_tool_module_registry_test.cpp contract)
mira_add_test(mira_m7_tool_module_exposure_test m7/m7_tool_module_exposure_test.cpp contract)
mira_add_test(mira_m7_tool_module_mcp_test m7/m7_tool_module_mcp_test.cpp contract)

# The TR0 stable reference layer (DEC-040) is a workflow-plane contract: it
# consumes WorkflowDefinition plus the core exposure view, so it compiles into
# Mira::workflow and its gate matrix links the workflow library like the
# M9/M14 helpers do.
function(mira_add_m7_reference_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        Mira::core Mira::workflow Mira::simulator_adapter executor::executor)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${name} SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/executor/include")
    mira_enable_warnings(${name})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
    endif()
    mira_enable_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "contract" TIMEOUT 90)
endfunction()
mira_add_m7_reference_test(mira_m7_tool_reference_test m7/m7_tool_reference_test.cpp)
mira_add_m7_reference_test(mira_m7_tool_skill_test m7/m7_tool_skill_test.cpp)
# TR2 runtime wiring (DEC-040 section 18): IR v1.1 reference expression, the
# mount table, the publish-gate auto extraction, the create_run tool-compat
# admission gate with its Degraded event, the skill execution adapters and the
# Procedure memory sync; gates M7-TR2-G1..G6.
mira_add_m7_reference_test(mira_m7_tool_runtime_wiring_test m7/m7_tool_runtime_wiring_test.cpp)
mira_add_m7_reference_test(mira_m7_tool_runtime_skill_test m7/m7_tool_runtime_skill_test.cpp)
