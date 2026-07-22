cmake_minimum_required(VERSION 3.10)
add_executable(msm_web_v3 main_hwdaq.cpp web/msm_daq_hw.html)
target_compile_definitions(msm_web_v3 PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_web_v3 PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/include)
target_include_directories(msm_web_v3 PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_web_v3 PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/lib)
target_link_directories(msm_web_v3 PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_web_v3
    module_daq
    module_process
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread cpp-httplib
)
