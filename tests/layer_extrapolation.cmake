# Runs one call-chain scenario with extrapolation on (`extrapolate=1` in the
# ofxr_bridge.ini beside the layer DLL) and checks, from the fake runtime's
# log, that each pair went down as the real frame first and the synthetic a
# display period later. The scenario's own assertions expect the
# interpolating order, so they are not held.
foreach(required_variable IN ITEMS LAYER_DLL CALL_CHAIN EXTRAPOLATE_INI WORK_DIR MODE)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not provided")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${LAYER_DLL}" DESTINATION "${WORK_DIR}")
file(COPY "${EXTRAPOLATE_INI}" DESTINATION "${WORK_DIR}")
get_filename_component(ini_name "${EXTRAPOLATE_INI}" NAME)
file(RENAME "${WORK_DIR}/${ini_name}" "${WORK_DIR}/ofxr_bridge.ini")

get_filename_component(layer_name "${LAYER_DLL}" NAME)
set(mode_arguments "${MODE}")
if(MODE STREQUAL "default")
    set(mode_arguments "")
endif()
execute_process(
    COMMAND "${CALL_CHAIN}"
        "${WORK_DIR}/${layer_name}"
        "${WORK_DIR}/fake-runtime.log"
        ${mode_arguments}
    RESULT_VARIABLE call_chain_result
    OUTPUT_VARIABLE call_chain_output
    ERROR_VARIABLE call_chain_error)

file(STRINGS "${WORK_DIR}/fake-runtime.log" end_frames
    REGEX "downstream end frame target=[a-z]+ time=[0-9]+")
# Both orders alternate once running, so the start tells them apart: the
# prime is a real frame alone, and then extrapolation hands over the pair's
# real frame before its synthetic, where interpolation hands the synthetic
# over first. The presenter may repeat the last frame handed over while the
# next pair is not ready, which extrapolating is a synthetic.
set(previous_target "")
set(previous_time 0)
set(ordered_pairs 0)
set(real_frames 0)
foreach(line IN LISTS end_frames)
    string(REGEX REPLACE ".*target=([a-z]+) time=([0-9]+).*" "\\1" target "${line}")
    string(REGEX REPLACE ".*target=([a-z]+) time=([0-9]+).*" "\\2" time "${line}")
    if(target STREQUAL "current")
        math(EXPR real_frames "${real_frames} + 1")
    endif()
    if(target STREQUAL "synthetic")
        if(ordered_pairs EQUAL 0 AND real_frames LESS 2)
            message(FATAL_ERROR
                "The first synthetic went down before its real frame:\n${line}")
        endif()
        if(NOT previous_target STREQUAL "current" AND NOT previous_target STREQUAL "synthetic")
            message(FATAL_ERROR
                "A synthetic went down after '${previous_target}', not after a real frame:\n${line}")
        endif()
        # Later than what went before: the scenarios' runtimes keep their own
        # periods, and some jitter them.
        if(NOT time GREATER previous_time)
            message(FATAL_ERROR
                "A synthetic went down at ${time}, not after the frame before at ${previous_time}:\n${line}")
        endif()
        if(previous_target STREQUAL "current")
            math(EXPR ordered_pairs "${ordered_pairs} + 1")
        endif()
    endif()
    set(previous_target "${target}")
    set(previous_time "${time}")
endforeach()
# A scenario can end at its own interpolation-order assertions after a pair.
if(ordered_pairs LESS 1)
    message(FATAL_ERROR
        "Expected real-then-synthetic pairs, found ${ordered_pairs} "
        "(call chain ${call_chain_result}):\n${call_chain_output}\n${call_chain_error}")
endif()
message("${ordered_pairs} pairs went down real frame first")
