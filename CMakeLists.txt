cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartconfig_ai_rtos)

# The SDK's bootloader is a separate CMake build and retains its -Og default.
# Optimize only the application libraries to fit the ESP8266's 1 MiB mapping.
idf_build_get_property(app_components BUILD_COMPONENTS)
foreach(component ${app_components})
    idf_component_get_property(component_lib ${component} COMPONENT_LIB)
    get_target_property(component_type ${component_lib} TYPE)
    if(NOT component_type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${component_lib} PRIVATE -Os)
    endif()
endforeach()
