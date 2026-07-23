cmake_minimum_required(VERSION 3.10)
project(MSM_DAQ LANGUAGES CXX)

cmake_policy(SET CMP0079 NEW)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Each version's binaries go into its own build/ directory
# (set per-target RUNTIME_OUTPUT_DIRECTORY in subdirectory CMakeLists.txt)

# ── Framework core ──
add_subdirectory(module_core)

# ── Shared libraries ──
add_subdirectory(extern/circular_buffer)
add_subdirectory(module_daq)
add_subdirectory(module_process)
add_subdirectory(module_storage)
add_subdirectory(module_simsource)

# ── Main executables ──
add_subdirectory(module_main_v1)   # msm_cli_simsrc, msm_cli_hwdaq, msm_ipc_shell
add_subdirectory(module_main_v3)   # msm_webui (IPC-to-HTTP bridge)
