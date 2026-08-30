set(INSTALL_DIR "${BINARY_DIR}/prefix")
set(CONSUMER_BUILD_DIR "${BINARY_DIR}/consumer")
file(REMOVE_RECURSE "${BINARY_DIR}")

set(SANITIZER_FLAG "")
if(MIRA_ENABLE_ASAN)
    set(SANITIZER_FLAG "-fsanitize=address")
elseif(MIRA_ENABLE_UBSAN)
    set(SANITIZER_FLAG "-fsanitize=undefined")
elseif(MIRA_ENABLE_TSAN)
    set(SANITIZER_FLAG "-fsanitize=thread")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BUILD_DIR}" --prefix "${INSTALL_DIR}"
            --config "${BUILD_CONFIG}"
    RESULT_VARIABLE INSTALL_RESULT
)
if(NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "Mira install failed")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/consumer" -B "${CONSUMER_BUILD_DIR}"
            -G "${GENERATOR}" -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
            -DCMAKE_PREFIX_PATH=${INSTALL_DIR}
            -DCMAKE_CXX_FLAGS=${SANITIZER_FLAG}
            -DCMAKE_EXE_LINKER_FLAGS=${SANITIZER_FLAG}
    RESULT_VARIABLE CONFIGURE_RESULT
)
if(NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Installed consumer configure failed")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD_DIR}" --parallel 2
    RESULT_VARIABLE BUILD_RESULT
)
if(NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Installed consumer build failed")
endif()
execute_process(
    COMMAND "${CONSUMER_BUILD_DIR}/mira_installed_consumer"
    RESULT_VARIABLE RUN_RESULT
)
if(NOT RUN_RESULT EQUAL 0)
    message(FATAL_ERROR "Installed consumer returned ${RUN_RESULT}")
endif()
