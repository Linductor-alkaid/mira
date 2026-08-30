file(REMOVE_RECURSE "${BINARY_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/headers" -B "${BINARY_DIR}"
            -G "${GENERATOR}" -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
            -DMIRA_INCLUDE_DIR=${SOURCE_DIR}/include
    RESULT_VARIABLE CONFIGURE_RESULT
)
if(NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Public header check configure failed")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" --parallel 2
    RESULT_VARIABLE BUILD_RESULT
)
if(NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Public header check build failed")
endif()
