find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format-19 clang-format-18 clang-format-17)
if(NOT CLANG_FORMAT_EXECUTABLE)
    message(FATAL_ERROR "clang-format was not found")
endif()

file(GLOB_RECURSE MIRA_FORMAT_FILES
    "${ROOT_DIR}/include/*.hpp"
    "${ROOT_DIR}/src/*.cpp"
    "${ROOT_DIR}/adapters/*.cpp"
    "${ROOT_DIR}/tests/*.cpp"
    "${ROOT_DIR}/examples/*.cpp"
)
foreach(FILE_PATH IN LISTS MIRA_FORMAT_FILES)
    execute_process(
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror "${FILE_PATH}"
        RESULT_VARIABLE FORMAT_RESULT
    )
    if(NOT FORMAT_RESULT EQUAL 0)
        message(FATAL_ERROR "Formatting check failed: ${FILE_PATH}")
    endif()
endforeach()
