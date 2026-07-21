add_executable(msm_daq_storage main.cpp)
target_compile_definitions(msm_daq_storage PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_daq_storage PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_daq_storage PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_daq_storage
    module_daq
    module_process
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread
)
