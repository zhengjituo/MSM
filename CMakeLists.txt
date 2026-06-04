cmake_minimum_required(VERSION 3.10)
project(MSM_DAQ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk_include)
set(MSM_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/msm/include)

add_subdirectory(msm)

add_library(user_circular_buffer STATIC
    extern/circular_buffer/user_circular_buffer.cpp
)
target_include_directories(user_circular_buffer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extern/circular_buffer)

add_library(daq_module STATIC
    module_daq/daq_module.cpp
)
target_link_libraries(daq_module
    msm_core
    user_circular_buffer
)
target_include_directories(daq_module
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/module_daq
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MSM_INCLUDE}
    PRIVATE ${SDK_DIR}
)
target_link_directories(daq_module PRIVATE ${SDK_DIR})
target_link_libraries(daq_module CxDeviceCore xdma_api2 pthread)

add_library(consumer_module STATIC
    module_consumer/consumer_module.cpp
)
target_link_libraries(consumer_module
    msm_core
    user_circular_buffer
)
target_include_directories(consumer_module
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/module_consumer
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MSM_INCLUDE}
)

add_executable(msm_daq
    Module_main/main.cpp
)
target_link_libraries(msm_daq
    daq_module
    consumer_module
    msm_core
    user_circular_buffer
)
target_include_directories(msm_daq PRIVATE ${SDK_DIR})
target_link_directories(msm_daq PRIVATE ${SDK_DIR})
target_link_libraries(msm_daq CxDeviceCore xdma_api2 pthread)
