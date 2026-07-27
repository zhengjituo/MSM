add_library(module_daq STATIC
    module_daq.cpp
)
target_link_libraries(module_daq PUBLIC msm_core user_circular_buffer module_config)
target_include_directories(module_daq
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk_include
)
target_link_directories(module_daq PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk_include)
target_link_libraries(module_daq PRIVATE CxDeviceCore xdma_api2 pthread)
