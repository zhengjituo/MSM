add_executable(msm_daq_sim main.cpp)
target_compile_definitions(msm_daq_sim PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(msm_daq_sim
    module_simsource
    module_process_full
    module_storage
    module_daq
    msm_core
    user_circular_buffer
)
