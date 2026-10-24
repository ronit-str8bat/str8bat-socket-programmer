find_package(Git QUIET)
set(PROBE_VERSION "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --exact-match --tags
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE GIT_RESULT
        OUTPUT_VARIABLE GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT GIT_RESULT EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${SOURCE_DIR}"
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_VARIABLE GIT_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if(GIT_RESULT EQUAL 0 AND NOT GIT_VERSION STREQUAL "")
        set(PROBE_VERSION "${GIT_VERSION}")
    endif()
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/version.h.in" "${OUTPUT_DIR}/version.h" @ONLY)
