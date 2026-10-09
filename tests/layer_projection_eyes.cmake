# Runs the call chain with a swapchain per eye (split-eye, both views in one
# projection layer: XRFG_TEST_SPLIT_EYE_ONE_LAYER) and checks from the flight
# log that the layer learns which eye each one is (presenter_transition
# 750, b the eye + 1): the eye that picks its own DLSS evaluation when both
# eyes' images match every evaluation by size. The double-wide run, one
# swapchain holding both eyes, records none.
foreach(required_variable IN ITEMS LAYER_DLL CALL_CHAIN ENABLED_INI WORK_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not provided")
    endif()
endforeach()

get_filename_component(layer_name "${LAYER_DLL}" NAME)
set(ENV{XRFG_TEST_SPLIT_EYE_ONE_LAYER} 1)
foreach(mode IN ITEMS split-eye double-wide)
    set(run_dir "${WORK_DIR}/${mode}")
    file(REMOVE_RECURSE "${run_dir}")
    file(MAKE_DIRECTORY "${run_dir}")
    file(COPY "${LAYER_DLL}" DESTINATION "${run_dir}")
    file(READ "${ENABLED_INI}" ini)
    file(WRITE "${run_dir}/ofxr_bridge.ini" "${ini}")
    execute_process(
        COMMAND "${CALL_CHAIN}"
            "${run_dir}/${layer_name}"
            "${run_dir}/fake-runtime.log"
            ${mode}
        RESULT_VARIABLE call_chain_result
        OUTPUT_VARIABLE call_chain_output
        ERROR_VARIABLE call_chain_error)
    if(NOT call_chain_result EQUAL 0)
        message(FATAL_ERROR
            "The ${mode} call chain failed (${call_chain_result}):\n"
            "${call_chain_output}\n${call_chain_error}")
    endif()
    file(GLOB flight_logs "${run_dir}/ofxr-bridge-flight-*.log")
    list(LENGTH flight_logs flight_log_count)
    if(NOT flight_log_count EQUAL 1)
        message(FATAL_ERROR "Expected one ${mode} flight log, found ${flight_log_count}")
    endif()
    list(GET flight_logs 0 flight_log)
    file(STRINGS "${flight_log}" eyes REGEX "op=presenter_transition result=750 ")
    set(left 0)
    set(right 0)
    foreach(line IN LISTS eyes)
        if(line MATCHES " b=1 ")
            math(EXPR left "${left} + 1")
        elseif(line MATCHES " b=2 ")
            math(EXPR right "${right} + 1")
        endif()
    endforeach()
    list(LENGTH eyes records)
    if(mode STREQUAL "split-eye")
        if(NOT left EQUAL 1 OR NOT right EQUAL 1)
            message(FATAL_ERROR
                "The split-eye swapchains were not each named one eye once "
                "(${left} left, ${right} right, ${records} records):\n${eyes}")
        endif()
    elseif(NOT records EQUAL 0)
        message(FATAL_ERROR
            "A swapchain holding both eyes was named an eye:\n${eyes}")
    endif()
endforeach()
message("Each split-eye swapchain was named its eye; the double-wide one none")
