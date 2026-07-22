add_executable(msm_web_hwdaq main_hwdaq.cpp web/msm_daq_hw.html)
# (待建: msm_web_simsrc with main_simsrc.cpp)
target_compile_definitions(msm_web_hwdaq PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_web_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/include)
target_include_directories(msm_web_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_web_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/lib)
target_link_directories(msm_web_hwdaq PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_web_hwdaq
    module_daq
    module_process
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread cpp-httplib
)
