set(TARGET_DIR ${CMAKE_SOURCE_DIR}/main_cli/build)

set(CONFIG_HANDOFF_SRC config_handoff.cpp)

add_executable(msm_cli_simsrc main_simsrc.cpp ${CONFIG_HANDOFF_SRC})
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

add_executable(msm_cli_hwdaq main_hwdaq.cpp ${CONFIG_HANDOFF_SRC})
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

# PyDM subprocess 调用的配置编辑 CLI（meta / get / save / save-app）
add_executable(msm_cli_config msm_cli_config.cpp)
set_target_properties(msm_cli_config PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_compile_definitions(msm_cli_config PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(msm_cli_config
    module_config
    msm_core
)

add_executable(msm_ipc_shell msm_ipc_shell.cpp)
set_target_properties(msm_ipc_shell PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_link_libraries(msm_ipc_shell pthread)

add_executable(test_parse ${CMAKE_SOURCE_DIR}/module_process/test_parse.cpp
                          ${CMAKE_SOURCE_DIR}/module_process/frame_parser.cpp)
set_target_properties(test_parse PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_include_directories(test_parse PRIVATE ${CMAKE_SOURCE_DIR}/module_process)
