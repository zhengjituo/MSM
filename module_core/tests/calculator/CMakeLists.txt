cmake_minimum_required(VERSION 3.15)
project(module_calculator VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MSM_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../module_core)
if(NOT TARGET msm_core)
    add_subdirectory(${MSM_CORE_DIR} msm_core_build)
endif()

set(EPICS_BASE ${MSM_CORE_DIR}/extern/epics)

add_executable(calculator_main
    src/AddModule.cpp
    src/MultiplyModule.cpp
    src/WaveModule.cpp
    src/CalculatorMain.cpp
)

target_link_libraries(calculator_main msm_core)

target_include_directories(calculator_main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSM_CORE_DIR}/include
)

add_executable(calc_alpha
    src/CalcAlpha.cpp
    src/AddModule.cpp
)
target_link_libraries(calc_alpha msm_core)
target_compile_definitions(calc_alpha PRIVATE
    BINARY_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(calc_alpha PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSM_CORE_DIR}/include
)

add_executable(calc_beta
    src/CalcBeta.cpp
    src/MultiplyModule.cpp
)
target_link_libraries(calc_beta msm_core)
target_compile_definitions(calc_beta PRIVATE
    BINARY_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(calc_beta PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSM_CORE_DIR}/include
)

# ─── DataChannel Perf Test ────────────────────────────────────
add_executable(dc_test_producer src/DCTestProducer.cpp)
target_link_libraries(dc_test_producer
    ${MSM_CORE_DIR}/extern/lib/libzmq_minimal.a
    pthread
    dl
    rt
)
target_include_directories(dc_test_producer PRIVATE
    ${MSM_CORE_DIR}/extern/include
)

add_executable(dc_test_consumer src/DCTestConsumer.cpp)
target_link_libraries(dc_test_consumer
    ${MSM_CORE_DIR}/extern/lib/libzmq_minimal.a
    pthread
    dl
    rt
)
target_include_directories(dc_test_consumer PRIVATE
    ${MSM_CORE_DIR}/extern/include
)

message("Module Calculator v${PROJECT_VERSION} (IPC mode)")