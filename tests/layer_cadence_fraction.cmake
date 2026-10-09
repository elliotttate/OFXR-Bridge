# Runs the promise-shown-time call chain with stamps that do not follow the
# frames' slots - every ninth frame stamped with the display time of the frame
# before it, as Unreal Engine 4's OpenXR plugin does now and then
# (XRFG_TEST_REPEAT_STAMP_EVERY=9), and the rate halved part way through, as
# SteamVR does (XRFG_TEST_THROTTLE_AFTER_WAITS=150) - and checks from the
# layer's synthesis_fraction records that every pair's synthetic is placed at
# the cadence's share, the midpoint, whatever the stamps say.
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
# Half the default run is enough for every check here.
set(ENV{XRFG_TEST_PROMISE_FRAMES} 160)
set(ENV{XRFG_TEST_REPEAT_STAMP_EVERY} 9)
set(ENV{XRFG_TEST_THROTTLE_AFTER_WAITS} 150)
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

file(STRINGS "${flight_log}" fractions REGEX "op=synthesis_fraction ")
set(pairs 0)
set(throttled 0)
set(off 0)
set(off_lines "")
foreach(line IN LISTS fractions)
    math(EXPR pairs "${pairs} + 1")
    # a is the interval between the stamps in us: 40 ms at half the rate.
    if(line MATCHES " a=(39[0-9][0-9][0-9]|40[0-9][0-9][0-9]) ")
        math(EXPR throttled "${throttled} + 1")
    endif()
    string(REGEX REPLACE ".* result=(-?[0-9]+) .*" "\\1" fraction "${line}")
    if(NOT fraction EQUAL 5000)
        math(EXPR off "${off} + 1")
        string(APPEND off_lines "${line}\n")
    endif()
endforeach()
string(REGEX MATCH "repeated stamps ([0-9]+)" repeated "${call_chain_output}")
set(repeated "${CMAKE_MATCH_1}")
if(pairs LESS 100 OR throttled LESS 30 OR repeated STREQUAL "" OR repeated LESS 6)
    message(FATAL_ERROR
        "Only ${pairs} pairs recorded a synthesis fraction (${throttled} at half "
        "the rate), with '${repeated}' repeated stamps")
endif()
if(off GREATER 0)
    message(FATAL_ERROR
        "${off} of ${pairs} pairs placed their synthetic away from the "
        "midpoint:\n${off_lines}")
endif()
message("${pairs} pairs (${throttled} at half the rate, ${repeated} repeated "
        "stamps), all at the midpoint")
