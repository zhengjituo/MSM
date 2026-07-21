add_executable(msm_cli_simsrc main_simsrc.cpp)
target_compile_definitions(msm_cli_simsrc PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(msm_cli_simsrc
    module_simsource
    module_process_full
    module_storage
    module_daq
    msm_core
    user_circular_buffer
)
