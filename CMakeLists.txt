cmake_minimum_required(VERSION 3.10)
project(MSM_DAQ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep all executables in build/ (not build/module_main_v*/)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ── Framework core ──
add_subdirectory(module_core)

# ── Shared libraries ──
add_subdirectory(extern/circular_buffer)
add_subdirectory(module_daq)
add_subdirectory(module_process)
add_subdirectory(module_process_full)
add_subdirectory(module_storage)
add_subdirectory(module_sim)

# ── Main executables ──
add_subdirectory(module_main_v1)   # msm_daq_sim
add_subdirectory(module_main_v2)   # msm_daq_display
add_subdirectory(module_main_v3)   # msm_daq_storage
