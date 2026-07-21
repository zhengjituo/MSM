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
add_subdirectory(module_simsource)

# ── Main executables ──
add_subdirectory(module_main_v1)   # msm_cli_simsrc  (V1 CLI + simsource)
add_subdirectory(module_main_v2)   # msm_web_hwdaq   (V2 WebUI + hardware)
add_subdirectory(module_main_v3)   # msm_cli_hwdaq   (V3 CLI + hardware + rec, 暂留)
