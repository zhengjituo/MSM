add_library(module_simsource STATIC
    module_simsource.cpp
)
target_link_libraries(module_simsource PUBLIC msm_core user_circular_buffer)
target_include_directories(module_simsource
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
