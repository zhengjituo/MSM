set(TARGET_DIR ${CMAKE_SOURCE_DIR}/module_main_v3/build)

add_executable(msm_webui main_hwdaq.cpp web/msm_daq_hw.html)
set_target_properties(msm_webui PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})
target_compile_definitions(msm_webui PRIVATE MSM_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(msm_webui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/include)
target_link_directories(msm_webui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/lib)
target_link_libraries(msm_webui cpp-httplib pthread)
