cmake_minimum_required(VERSION 3.10)

project(msm_epics CXX)

set(TARGET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build)

set(EPICS_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/epics)
set(EPICS_LIBDIR ${EPICS_BASE}/lib/linux-x86_64)

add_executable(msm_epics main_epics.cpp)

set_target_properties(msm_epics PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${TARGET_DIR})

target_include_directories(msm_epics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/include
    ${EPICS_BASE}/include
    ${EPICS_BASE}/include/os/Linux
    ${EPICS_BASE}/include/compiler/gcc)

target_link_directories(msm_epics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../module_core/extern/lib
    ${EPICS_LIBDIR})

target_link_libraries(msm_epics PRIVATE
    ca Com dbCore dbRecStd pthread)

target_link_options(msm_epics PRIVATE
    "-Wl,-rpath,${EPICS_LIBDIR}")
