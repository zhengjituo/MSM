add_library(module_process_full STATIC
    module_process_full.cpp
)
target_link_libraries(module_process_full PUBLIC msm_core user_circular_buffer)
target_include_directories(module_process_full
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
