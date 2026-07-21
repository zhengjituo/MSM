add_library(module_storage STATIC
    module_storage.cpp
)
target_link_libraries(module_storage PUBLIC msm_core user_circular_buffer)
target_include_directories(module_storage
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
