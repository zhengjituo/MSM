add_executable(msm_tui_hwdaq main_tui.cpp)
target_compile_definitions(msm_tui_hwdaq PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_tui_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_tui_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_tui_hwdaq
    module_tui
    module_daq
    module_process_full
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread
)
