set(TARGET_DIR ${CMAKE_SOURCE_DIR}/main_cli/build)

add_executable(msm_cli_simsrc main_simsrc.cpp)
set_target_properties(msm_cli_simsrc PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_compile_definitions(msm_cli_simsrc PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(msm_cli_simsrc
    module_config
    module_simsource
    module_process
    module_storage
    module_daq
    msm_core
    user_circular_buffer
)

add_executable(msm_cli_hwdaq main_hwdaq.cpp)
set_target_properties(msm_cli_hwdaq PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_compile_definitions(msm_cli_hwdaq PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_cli_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_cli_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_cli_hwdaq
    module_config
    module_daq
    module_process
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread
)

add_executable(msm_ipc_shell msm_ipc_shell.cpp)
set_target_properties(msm_ipc_shell PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_link_libraries(msm_ipc_shell pthread)
