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

add_executable(msm_cli_hwdaq main_hwdaq.cpp)
target_compile_definitions(msm_cli_hwdaq PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_cli_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_cli_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_cli_hwdaq
    module_daq
    module_process_full
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread
)
