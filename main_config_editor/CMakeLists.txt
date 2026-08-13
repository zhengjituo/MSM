cmake_minimum_required(VERSION 3.10)
project(msm_config_editor CXX)

set(TARGET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build)

add_executable(msm_config_editor main_config_editor.cpp)
set_target_properties(msm_config_editor PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_include_directories(msm_config_editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/include
)
target_link_directories(msm_config_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/lib)
target_link_libraries(msm_config_editor PRIVATE module_config cpp-httplib pthread)
