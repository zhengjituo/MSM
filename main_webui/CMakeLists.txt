cmake_minimum_required(VERSION 3.10)
project(msm_webui CXX)

set(TARGET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build)

add_executable(msm_webui main_hwdaq.cpp web/msm_daq_hw.html)
set_target_properties(msm_webui PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_include_directories(msm_webui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/include)
target_link_directories(msm_webui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/lib)
target_link_libraries(msm_webui cpp-httplib pthread)
