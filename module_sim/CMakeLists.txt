add_library(module_sim STATIC
    module_sim.cpp
)
target_link_libraries(module_sim PUBLIC msm_core user_circular_buffer)
target_include_directories(module_sim
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
