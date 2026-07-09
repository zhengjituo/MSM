cmake_minimum_required(VERSION 3.15)
project(msm_core VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

option(MSM_ENABLE_EPICS "Enable EPICS CA support (PVServer)" ON)
option(MSM_ENABLE_ZMQ  "Enable ZMQ gateway (MSMGateway)" ON)
option(MSM_ENABLE_HTTP "Enable REST/Web server (RESTServer)" ON)

set(SOURCES
    src/Message.cpp
    src/MessageQueue.cpp
    src/NIStringParser.cpp
    src/Module.cpp
    src/MessageBus.cpp
    src/IPCServer.cpp
)

set(EXTERN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern)

if(MSM_ENABLE_EPICS)
    list(APPEND SOURCES src/PVServer.cpp)
endif()
if(MSM_ENABLE_ZMQ)
    list(APPEND SOURCES src/MSMGateway.cpp)
endif()
if(MSM_ENABLE_HTTP)
    list(APPEND SOURCES src/RESTServer.cpp)
endif()

add_library(msm_core STATIC ${SOURCES})

target_include_directories(msm_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${EXTERN_DIR}/include>
)

set(RPATH_DIRS "")
set(EXTRA_LIBS pthread dl rt)

if(MSM_ENABLE_EPICS)
    set(EPICS_BASE ${CMAKE_CURRENT_SOURCE_DIR}/extern/epics)
    set(EPICS_ARCH linux-x86_64)

    target_include_directories(msm_core
        PUBLIC
            $<BUILD_INTERFACE:${EPICS_BASE}/include>
            $<BUILD_INTERFACE:${EPICS_BASE}/include/compiler/gcc>
            $<BUILD_INTERFACE:${EPICS_BASE}/include/os/Linux>
    )

    target_link_directories(msm_core
        PUBLIC $<BUILD_INTERFACE:${EPICS_BASE}/lib/${EPICS_ARCH}>
    )

    list(APPEND EXTRA_LIBS dbCore dbRecStd Com ca)
    list(APPEND RPATH_DIRS ${EPICS_BASE}/lib/${EPICS_ARCH})
endif()

if(MSM_ENABLE_ZMQ)
    list(APPEND EXTRA_LIBS ${EXTERN_DIR}/lib/libzmq_minimal.a)
endif()

if(MSM_ENABLE_HTTP)
    list(APPEND EXTRA_LIBS ${EXTERN_DIR}/lib/libcpp-httplib.so)
    list(APPEND RPATH_DIRS ${EXTERN_DIR}/lib)
endif()

list(APPEND RPATH_DIRS ${EXTERN_DIR}/lib)

target_link_directories(msm_core
    PUBLIC $<BUILD_INTERFACE:${EXTERN_DIR}/lib>
)

target_link_libraries(msm_core PUBLIC ${EXTRA_LIBS})

if(RPATH_DIRS)
    string(JOIN ":" RPATH_VAL ${RPATH_DIRS})
    set_target_properties(msm_core PROPERTIES
        INTERFACE_LINK_OPTIONS "-Wl,-rpath,${RPATH_VAL}"
    )
endif()

set_target_properties(msm_core PROPERTIES
    OUTPUT_NAME "msm_core"
    DEBUG_POSTFIX "d"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_subdirectory(test)

install(TARGETS msm_core
    EXPORT msm_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION include
)

install(EXPORT msm_coreTargets
    FILE msm_coreTargets.cmake
    NAMESPACE msm::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/msm_core
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN *.h
)

configure_file(msm_coreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/msm_coreConfig.cmake
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/msm_coreConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/msm_core
)

message(STATUS "MSM Framework v${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  EPICS: ${MSM_ENABLE_EPICS}")
message(STATUS "  ZMQ:   ${MSM_ENABLE_ZMQ}")
message(STATUS "  HTTP:  ${MSM_ENABLE_HTTP}")
