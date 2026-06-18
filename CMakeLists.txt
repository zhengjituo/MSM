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

add_library(module_daq STATIC
    module_daq/module_daq.cpp
    module_daq/daq_config.cpp
)
target_link_libraries(module_daq
    msm_core
    user_circular_buffer
)
target_include_directories(module_daq
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/module_daq
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MSM_INCLUDE}
    PRIVATE ${SDK_DIR}
)
target_link_directories(module_daq PRIVATE ${SDK_DIR})
target_link_libraries(module_daq CxDeviceCore xdma_api2 pthread)

add_library(module_process STATIC
    module_process/module_process.cpp
)
target_link_libraries(module_process
    msm_core
    user_circular_buffer
)
target_include_directories(module_process
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/module_process
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MSM_INCLUDE}
)

add_library(module_storage STATIC
    module_storage/module_storage.cpp
)
target_link_libraries(module_storage
    msm_core
    user_circular_buffer
)
target_include_directories(module_storage
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/module_storage
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MSM_INCLUDE}
)

add_library(module_sim STATIC
    module_sim/module_sim.cpp
)
target_link_libraries(module_sim
    msm_core
    user_circular_buffer
)
target_include_directories(module_sim
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/module_sim
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MSM_INCLUDE}
)

add_executable(msm_daq_storage
    Module_main/main_storage.cpp
)
target_link_libraries(msm_daq_storage
    module_daq
    module_process
    module_storage
    msm_core
    user_circular_buffer
)
target_include_directories(msm_daq_storage PRIVATE ${SDK_DIR})
target_link_directories(msm_daq_storage PRIVATE ${SDK_DIR})
target_link_libraries(msm_daq_storage CxDeviceCore xdma_api2 pthread)

add_executable(msm_daq_sim
    Module_main/main_sim.cpp
)
target_include_directories(msm_daq_sim PRIVATE module_daq)
target_link_libraries(msm_daq_sim
    module_sim
    module_process
    module_storage
    module_daq
    msm_core
    user_circular_buffer
)
