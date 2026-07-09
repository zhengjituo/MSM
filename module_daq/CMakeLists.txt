cmake_minimum_required(VERSION 3.15)
project(module_daq VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MSM_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../module_core)
if(NOT TARGET msm_core)
    add_subdirectory(${MSM_CORE_DIR} msm_core_build)
endif()

set(CIRC_BUF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/circular_buffer)

add_library(user_circular_buffer STATIC
    ${CIRC_BUF_DIR}/user_circular_buffer.cpp
)
target_include_directories(user_circular_buffer PUBLIC ${CIRC_BUF_DIR})

add_library(module_sim STATIC
    module_sim.cpp
)
target_link_libraries(module_sim msm_core user_circular_buffer)
target_include_directories(module_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(module_process STATIC
    module_process.cpp
)
target_link_libraries(module_process msm_core user_circular_buffer)
target_include_directories(module_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(module_storage STATIC
    module_storage.cpp
)
target_link_libraries(module_storage msm_core user_circular_buffer)
target_include_directories(module_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(daq_config STATIC daq_config.cpp)
target_include_directories(daq_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(msm_daq_sim
    main_sim.cpp
)
target_compile_definitions(msm_daq_sim PRIVATE
    MSM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/.."
)
target_link_libraries(msm_daq_sim
    daq_config module_sim module_process module_storage msm_core user_circular_buffer
)
target_include_directories(msm_daq_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSM_DAQ_ENABLE_HARDWARE)
    add_library(module_daq STATIC
        module_daq.cpp
    )
    target_link_libraries(module_daq msm_core user_circular_buffer daq_config)
    target_include_directories(module_daq
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk_include
    )
    target_link_directories(module_daq PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk_include)
    target_link_libraries(module_daq CxDeviceCore xdma_api2 pthread)

    add_executable(msm_daq_storage
        main_storage.cpp
    )
    target_link_libraries(msm_daq_storage
        module_daq module_process module_storage daq_config msm_core user_circular_buffer
    )
    target_include_directories(msm_daq_storage
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk_include
    )
    target_link_directories(msm_daq_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk_include)
    target_link_libraries(msm_daq_storage CxDeviceCore xdma_api2 pthread)
endif()
