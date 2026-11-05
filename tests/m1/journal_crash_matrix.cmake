# Each writer exits without destructors; a separate verifier reopens its log.
if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}" OR
   NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}" OR NOT ROOT MATCHES "/journal-crash-matrix$")
    message(FATAL_ERROR "crash matrix requires its probe and dedicated absolute build root")
endif()
foreach(stage before_prepare after_prepare before_start after_start after_execute before_receipt after_receipt after_uncertain terminal)
    set(case_root "${ROOT}/${stage}")
    file(REMOVE_RECURSE "${case_root}")
    execute_process(COMMAND "${PROBE}" write "${case_root}" "${stage}"
        RESULT_VARIABLE written TIMEOUT 10)
    if(NOT written STREQUAL "0")
        message(FATAL_ERROR "writer ${stage}: ${written}")
    endif()
    execute_process(COMMAND "${PROBE}" verify "${case_root}" "${stage}"
        RESULT_VARIABLE verified TIMEOUT 10)
    if(NOT verified STREQUAL "0")
        message(FATAL_ERROR "recovery ${stage}: ${verified}")
    endif()
    file(REMOVE_RECURSE "${case_root}")
endforeach()
