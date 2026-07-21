add_library(module_process STATIC
    module_process.cpp
)
target_link_libraries(module_process PUBLIC msm_core user_circular_buffer)
target_include_directories(module_process
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
