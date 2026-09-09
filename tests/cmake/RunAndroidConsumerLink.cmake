# Cross-links the installed Mira package into tests/consumer with an Android
# toolchain (MNT-202609-22, BUG-20260909-001). Link-only by design: the gate
# proves every Workflow source file cross-compiles and that the installed
# package closes over Mira::workflow, executor and the durable stores. Running
# the consumer on a device stays out of scope and is tracked separately
# (MNT-202609-27).
#
# Inputs: SOURCE_DIR, PROJECT_BUILD_DIR, TOOLCHAIN_FILE and the optional
# CONSUMER_BUILD_DIR (defaults to "${PROJECT_BUILD_DIR}-consumer").

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT DEFINED PROJECT_BUILD_DIR)
    message(FATAL_ERROR "PROJECT_BUILD_DIR is required")
endif()
if(NOT DEFINED TOOLCHAIN_FILE)
    message(FATAL_ERROR "TOOLCHAIN_FILE is required")
endif()
if(NOT DEFINED CONSUMER_BUILD_DIR)
    set(CONSUMER_BUILD_DIR "${PROJECT_BUILD_DIR}-consumer")
endif()

set(INSTALL_DIR "${PROJECT_BUILD_DIR}-install")
file(REMOVE_RECURSE "${CONSUMER_BUILD_DIR}" "${INSTALL_DIR}")

# The install step is itself a gate: install(TARGETS) fails when any target
# in the MiraTargets export set was never built for this ABI, so a preset
# that skips mira_workflow cannot pass this script.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BUILD_DIR}" --prefix "${INSTALL_DIR}"
    RESULT_VARIABLE INSTALL_RESULT
)
if(NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "Mira Android install failed")
endif()

# CMAKE_FIND_ROOT_PATH is required in addition to CMAKE_PREFIX_PATH: under
# the Android toolchain package lookups are re-rooted, so without it the
# consumer cannot see MiraConfig.cmake or the installed executor package.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/consumer" -B "${CONSUMER_BUILD_DIR}"
            -G Ninja
            -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}
            -DCMAKE_PREFIX_PATH=${INSTALL_DIR}
            -DCMAKE_FIND_ROOT_PATH=${INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE CONFIGURE_RESULT
)
if(NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Android installed consumer configure failed")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD_DIR}" --parallel 2
    RESULT_VARIABLE BUILD_RESULT
)
if(NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Android installed consumer build failed")
endif()

set(CONSUMER_BINARY "${CONSUMER_BUILD_DIR}/mira_installed_consumer")
if(NOT EXISTS "${CONSUMER_BINARY}")
    message(FATAL_ERROR "Android consumer executable not found: ${CONSUMER_BINARY}")
endif()
message(STATUS "Android installed consumer linked: ${CONSUMER_BINARY}")
