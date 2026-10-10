foreach(required_variable IN ITEMS LAYER_DLL CALL_CHAIN ENABLED_INI WORK_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} was not provided")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${LAYER_DLL}" DESTINATION "${WORK_DIR}")
file(COPY "${ENABLED_INI}" DESTINATION "${WORK_DIR}")
file(RENAME
    "${WORK_DIR}/ofxr_bridge_logging_enabled.ini"
    "${WORK_DIR}/ofxr_bridge.ini")

get_filename_component(layer_name "${LAYER_DLL}" NAME)
execute_process(
    COMMAND "${CALL_CHAIN}"
        "${WORK_DIR}/${layer_name}"
        "${WORK_DIR}/fake-runtime.log"
    RESULT_VARIABLE call_chain_result
    OUTPUT_VARIABLE call_chain_output
    ERROR_VARIABLE call_chain_error)
if(NOT call_chain_result EQUAL 0)
    message(FATAL_ERROR
        "Flight-logging call chain failed (${call_chain_result}):\n"
        "${call_chain_output}\n${call_chain_error}")
endif()

file(GLOB flight_logs "${WORK_DIR}/ofxr-bridge-flight-*.log")
list(LENGTH flight_logs flight_log_count)
if(NOT flight_log_count EQUAL 1)
    message(FATAL_ERROR
        "Expected one OFXR flight log, found ${flight_log_count}")
endif()
list(GET flight_logs 0 flight_log)
file(READ "${flight_log}" contents)

foreach(required IN ITEMS
        "phase=B op=negotiation"
        "phase=E op=negotiation"
        "phase=B op=synthesis_initialize"
        "phase=E op=synthesis_initialize"
        "phase=B op=synthesis_pair"
        "phase=E op=synthesis_pair"
        "phase=I op=swapchain_create"
        "phase=I op=swapchain_eligibility"
        "phase=I op=projection_mapping"
        "phase=I op=generation_prepare"
        "phase=I op=session_state"
        "phase=I op=vram_usage result=1 "
        "phase=I op=vram_usage result=7 "
        "phase=I op=projection_view_rect"
        "phase=I op=deferred_capture result=0 "
        "phase=B op=downstream_first_end_frame"
        "phase=E op=downstream_first_end_frame"
        "phase=B op=internal_wait_frame"
        "phase=E op=internal_wait_frame"
        "phase=B op=internal_end_frame"
        "phase=E op=internal_end_frame")
    string(FIND "${contents}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Flight log is missing: ${required}")
    endif()
endforeach()

# reprojection_angle, for every frame of a pair as it is handed over. The fake
# runtime's head is where the application's poses put it at each time, so a
# real frame handed over at its own display time reads zero, and the first
# synthetic - submitted at the application's display time 400 with the pose
# halfway from frame 300's to frame 400's - reads half that turn: 2.005
# degrees, with synthetic_pose=interpolated (c=1).
foreach(required IN ITEMS
        "op=reprojection_angle result=1 dur_us=0 a=0 "
        "op=reprojection_angle result=2 dur_us=0 a=2005 ")
    string(FIND "${contents}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Flight log is missing: ${required}")
    endif()
endforeach()
string(REGEX MATCHALL "op=reprojection_angle result=[0-9] dur_us=0 a=[0-9]+ b=[0-9]+ c=0"
    real_pose_records "${contents}")
if(real_pose_records)
    message(FATAL_ERROR "reprojection_angle reports synthetic_pose=real: ${real_pose_records}")
endif()

message(STATUS "OFXR bridge flight logger call-chain contract verified")
