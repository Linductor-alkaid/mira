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
