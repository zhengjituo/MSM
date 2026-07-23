set(TARGET_DIR ${CMAKE_SOURCE_DIR}/module_main_v2/build)

add_executable(msm_hwdaq_v2 main_hwdaq.cpp)
set_target_properties(msm_hwdaq_v2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
# (待建: msm_simsrc_v2 with main_simsrc.cpp)
target_compile_definitions(msm_hwdaq_v2 PRIVATE MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(msm_hwdaq_v2 PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/include)
target_include_directories(msm_hwdaq_v2 PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_directories(msm_hwdaq_v2 PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/lib)
target_link_directories(msm_hwdaq_v2 PRIVATE ${CMAKE_SOURCE_DIR}/sdk_include)
target_link_libraries(msm_hwdaq_v2
    module_daq
    module_process
    module_storage
    msm_core
    user_circular_buffer
    CxDeviceCore xdma_api2 pthread
)
