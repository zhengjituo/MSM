cmake_minimum_required(VERSION 3.10)
project(MSM_DAQ LANGUAGES CXX)

cmake_policy(SET CMP0079 NEW)

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
add_subdirectory(module_simsource)

# ── Main executables ──
add_subdirectory(module_main_v1)   # msm_cli_simsrc (simsource+CLI), msm_cli_hwdaq (hwdaq+CLI)
add_subdirectory(module_main_v2)   # msm_web_hwdaq  (hwdaq+WebUI)

add_subdirectory(module_main_v3)   # msm_web_v3  (V3 WebUI，从 V2 备份起步)
