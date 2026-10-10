# Runs the promise-shown-time call chain with the flight log on and checks,
# from the layer's own presenter_content records, that the display time the
# application is promised for each frame comes to be the one its real frame
# actually goes down at: the application takes a period and a half to render,
# so its real frames first go down after the anchor, and the layer has to
# measure that and move the promise (a promise_correction record).
foreach(required_variable IN ITEMS LAYER_DLL CALL_CHAIN ENABLED_INI WORK_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not provided")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${LAYER_DLL}" DESTINATION "${WORK_DIR}")
# The logging-enabled ini bounds the log at 1 MB, which this run outgrows;
# a wrap would drop the correction record.
file(READ "${ENABLED_INI}" ini)
string(REPLACE "max_file_mb=1" "max_file_mb=16" ini "${ini}")
# EXTRA_INI: further sections, for the same check in another mode.
if(DEFINED EXTRA_INI)
    file(READ "${EXTRA_INI}" extra)
    string(APPEND ini "\n${extra}")
endif()
file(WRITE "${WORK_DIR}/ofxr_bridge.ini" "${ini}")

get_filename_component(layer_name "${LAYER_DLL}" NAME)
# SLOW_FRAME_EVERY: every Nth frame is handed over an application frame late,
# so the real frames go down in two groups a frame apart, as Kayak VR's do; the
# promise has to follow the larger group, which no window agrees on by nine in
# ten.
if(DEFINED SLOW_FRAME_EVERY)
    set(ENV{XRFG_TEST_SLOW_FRAME_EVERY} ${SLOW_FRAME_EVERY})
endif()
# POSE_LAG: the application submits the poses it located for its previous
# frame's time, as UEVR does. The promise follows those poses (pose_time_of),
# so each real frame goes down an application frame - two 10 ms periods at
# 2X - before the time it is stamped with, at the time its poses are for.
set(expected_late 0)
if(DEFINED POSE_LAG)
    set(ENV{XRFG_TEST_POSE_LAG} 1)
    set(expected_late -20000000)
endif()
# The correction follows two windows that agree: a run long enough that one
# made late on a loaded machine still leaves frames to judge it by.
set(ENV{XRFG_TEST_PROMISE_FRAMES} 400)
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

# A steady session never has its phase moved (observe_admission_wait).
file(STRINGS "${flight_log}" rephases REGEX "op=presenter_transition result=720 ")
if(rephases)
    message(FATAL_ERROR "The application's phase was moved in a steady session:
${rephases}")
endif()
file(STRINGS "${flight_log}" corrections REGEX "op=promise_correction ")
set(corrected_to 0)
foreach(line IN LISTS corrections)
    string(REGEX REPLACE ".* a=([0-9]+) .*" "\\1" corrected_to "${line}")
endforeach()
if(corrected_to LESS 1)
    message(FATAL_ERROR
        "The promise was never moved later; the real frames went down where "
        "first promised, so this run does not test the correction:\n${corrections}")
endif()

# The real frames of generated pairs (kind 1), after the last correction:
# each one's display time (c) against its promise (b), in a period of 10 ms.
file(STRINGS "${flight_log}" lines REGEX "op=(promise_correction|presenter_content result=1) ")
set(after_correction FALSE)
set(counted 0)
set(on_time 0)
set(skipped 0)
foreach(line IN LISTS lines)
    if(line MATCHES "op=promise_correction ")
        set(after_correction TRUE)
        set(counted 0)
        set(on_time 0)
        set(skipped 0)
        continue()
    endif()
    if(NOT after_correction)
        continue()
    endif()
    # Frames promised before the correction are still in flight.
    if(skipped LESS 8)
        math(EXPR skipped "${skipped} + 1")
        continue()
    endif()
    string(REGEX REPLACE ".* b=([0-9]+) c=([0-9]+).*" "\\1" promised "${line}")
    string(REGEX REPLACE ".* b=([0-9]+) c=([0-9]+).*" "\\2" shown "${line}")
    if(promised EQUAL 0)
        continue()
    endif()
    math(EXPR late "${shown} - ${promised} - (${expected_late})")
    math(EXPR counted "${counted} + 1")
    if(late GREATER -5000000 AND late LESS 5000000)
        math(EXPR on_time "${on_time} + 1")
    endif()
endforeach()
# A busy machine can move the application's timing late in the run, and the
# correction with it; what follows the last one is what is judged.
if(counted LESS 24)
    message(FATAL_ERROR
        "Only ${counted} real frames went down after the correction to "
        "${corrected_to} periods")
endif()
if(DEFINED SLOW_FRAME_EVERY)
    # Each slow frame goes down a frame late, and the frame after it with it.
    math(EXPR required "${counted} * 6 / 10")
else()
    math(EXPR required "${counted} * 9 / 10")
endif()
if(on_time LESS required)
    message(FATAL_ERROR
        "After the correction to ${corrected_to} periods only ${on_time} of "
        "${counted} real frames went down within half a period of their "
        "promise")
endif()
message("Promise moved ${corrected_to} periods later; ${on_time} of ${counted} "
        "real frames then went down at the time promised")
