add_library(msm_core STATIC
    src/Message.cpp
    src/NIStringParser.cpp
    src/Module.cpp
    src/MessageBus.cpp
    src/IPCServer.cpp
)

target_include_directories(msm_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(msm_core pthread)
