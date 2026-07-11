cmake_minimum_required(VERSION 3.16)
project(mqtt_device_suite LANGUAGES CXX)

add_executable(mqtt_device_suite device_suite.cpp)
target_include_directories(mqtt_device_suite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mqtt_device_suite PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(mqtt_device_suite PRIVATE ws2_32)
endif()
