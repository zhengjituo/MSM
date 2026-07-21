cmake_minimum_required(VERSION 3.15)
project(module_datachannel VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MSM_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../module_core)

if(NOT TARGET msm_core)
    add_subdirectory(${MSM_CORE_DIR} msm_core_build)
endif()

set(SOURCES
    src/DataChannel.cpp
    src/DCProducerModule.cpp
    src/DCConsumerModule.cpp
)

# Alpha: producer
add_executable(dc_alpha ${SOURCES} src/dc_alpha.cpp)
target_link_libraries(dc_alpha msm_core)
target_include_directories(dc_alpha PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSM_CORE_DIR}/include
)
target_compile_definitions(dc_alpha PRIVATE
    CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# Beta: consumer
add_executable(dc_beta ${SOURCES} src/dc_beta.cpp)
target_link_libraries(dc_beta msm_core)
target_include_directories(dc_beta PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSM_CORE_DIR}/include
)
target_compile_definitions(dc_beta PRIVATE
    CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

message("DataChannel Test Suite")
