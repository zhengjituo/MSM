add_library(module_config
    config_impl.cpp
    mod_config.cpp
)
target_include_directories(module_config
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(module_config PUBLIC msm_core)
