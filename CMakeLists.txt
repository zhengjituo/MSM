cmake_minimum_required(VERSION 3.10)
project(MSM_DAQ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk_include)

add_subdirectory(module_core)

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
)

add_executable(msm_daq_testbench
    module_daq/test_bench_e2e.cpp
)
target_include_directories(msm_daq_testbench
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/module_core/extern/include
)
target_link_directories(msm_daq_testbench
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/module_core/extern/lib
)
target_link_libraries(msm_daq_testbench
    pthread
    cpp-httplib
)

add_executable(msm_daq_storage
    module_main/main_storage.cpp
)
target_compile_definitions(msm_daq_storage PRIVATE
    MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
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
    module_main/main_sim.cpp
)
target_compile_definitions(msm_daq_sim PRIVATE
    MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)
target_include_directories(msm_daq_sim PRIVATE module_daq)
target_include_directories(msm_daq_sim PRIVATE module_process_full)
target_link_libraries(msm_daq_sim
    module_sim
    module_process_full
    module_storage
    module_daq
    msm_core
    user_circular_buffer
)

add_library(module_process_full STATIC
    module_process_full/module_process_full.cpp
)
target_link_libraries(module_process_full
    msm_core
    user_circular_buffer
)
target_include_directories(module_process_full
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/module_process_full
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(msm_daq_display
    module_main_full/main_full.cpp
)
target_compile_definitions(msm_daq_display PRIVATE
    MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(msm_daq_display
    module_daq
    module_process_full
    module_storage
    msm_core
    user_circular_buffer
)
target_include_directories(msm_daq_display PRIVATE ${SDK_DIR})
target_include_directories(msm_daq_display PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/include)
target_link_directories(msm_daq_display PRIVATE ${SDK_DIR})
target_link_directories(msm_daq_display PRIVATE ${CMAKE_SOURCE_DIR}/module_core/extern/lib)
target_link_libraries(msm_daq_display CxDeviceCore xdma_api2 pthread cpp-httplib)
