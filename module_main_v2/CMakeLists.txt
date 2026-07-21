add_executable(msm_daq_display main.cpp web/msm_daq_hw.html)
target_compile_definitions(msm_daq_display PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_daq_display PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/include)
target_include_directories(msm_daq_display PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_daq_display PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/lib)
target_link_directories(msm_daq_display PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_daq_display
    module_daq
    module_process_full
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread cpp-httplib
)
