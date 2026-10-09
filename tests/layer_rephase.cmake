# Runs the promise-shown-time call chain with XRFG_TEST_ADMISSION_WAIT_MS
# making every pair look as if it had waited most of a display period at
# admission, and checks from the flight log that the layer moves the
# application's phase (presenter_transition 720): once eight late pairs in a
# row say so, with the pair hold that follows holding a presenter frame
# longer, and then - since the waits stay where they were - again only after
# a run twice as long.
foreach(required_variable IN ITEMS LAYER_DLL CALL_CHAIN ENABLED_INI WORK_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not provided")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${LAYER_DLL}" DESTINATION "${WORK_DIR}")
file(READ "${ENABLED_INI}" ini)
string(REPLACE "max_file_mb=1" "max_file_mb=16" ini "${ini}")
file(WRITE "${WORK_DIR}/ofxr_bridge.ini" "${ini}")

get_filename_component(layer_name "${LAYER_DLL}" NAME)
set(ENV{XRFG_TEST_ADMISSION_WAIT_MS} 8)
execute_process(
    COMMAND "${CALL_CHAIN}"
        "${WORK_DIR}/${layer_name}"
        "${WORK_DIR}/fake-runtime.log"
        promise-shown-time
    RESULT_VARIABLE call_chain_result
    OUTPUT_VARIABLE call_chain_output
    ERROR_VARIABLE call_chain_error)
if(NOT call_chain_result EQUAL 0)
    message(FATAL_ERROR
        "The promise-shown-time call chain failed (${call_chain_result}):\n"
        "${call_chain_output}\n${call_chain_error}")
endif()

file(GLOB flight_logs "${WORK_DIR}/ofxr-bridge-flight-*.log")
list(LENGTH flight_logs flight_log_count)
if(NOT flight_log_count EQUAL 1)
    message(FATAL_ERROR "Expected one OFXR flight log, found ${flight_log_count}")
endif()
list(GET flight_logs 0 flight_log)

file(STRINGS "${flight_log}" lines
    REGEX "op=presenter_transition result=720 |op=presenter_pair_release ")
set(moves "")
set(first_hold "")
set(other_holds 0)
set(other_long_holds 0)
set(after_first FALSE)
foreach(line IN LISTS lines)
    if(line MATCHES "result=720 ")
        string(REGEX REPLACE ".* c=([0-9]+).*" "\\1" windows "${line}")
        list(APPEND moves "${windows}")
        list(LENGTH moves move_count)
        if(move_count EQUAL 1)
            set(after_first TRUE)
        endif()
        continue()
    endif()
    # presenter_pair_release: a is how long the application was held, in
    # microseconds.
    string(REGEX REPLACE ".* a=([0-9]+) .*" "\\1" held "${line}")
    if(after_first)
        set(first_hold "${held}")
        set(after_first FALSE)
    else()
        math(EXPR other_holds "${other_holds} + 1")
        if(held GREATER 3000)
            math(EXPR other_long_holds "${other_long_holds} + 1")
        endif()
    endif()
endforeach()

list(LENGTH moves move_count)
if(move_count LESS 2)
    message(FATAL_ERROR
        "Expected the phase to be moved at least twice with every pair late at "
        "admission; the moves were '${moves}'")
endif()
list(GET moves 0 first_windows)
list(GET moves 1 second_windows)
if(NOT first_windows EQUAL 1 OR NOT second_windows EQUAL 2)
    message(FATAL_ERROR
        "Expected the first move after one run and the next after a run twice "
        "as long, since the first left the waits where they were; the run "
        "lengths were '${moves}'")
endif()
if(first_hold STREQUAL "" OR first_hold LESS 3000)
    message(FATAL_ERROR
        "The pair hold after the first move held ${first_hold} us; it should "
        "have run a presenter frame longer (${other_long_holds} of "
        "${other_holds} other holds were over 3 ms)")
endif()
message("Phase moved ${move_count} times (runs ${moves}); the hold after "
        "the first held ${first_hold} us")
