cmake_minimum_required(VERSION 3.16)
project(mqtt_broker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mqtt_broker STATIC
    src/varint.cpp
    src/wire.cpp
    src/message_pool.cpp
    src/debug_log.cpp
    src/protocol_error.cpp
    src/subscription_options.cpp
    src/topic_trie.cpp
    src/topic_intern.cpp
    src/property_pool.cpp
    src/property_rules.cpp
    src/parser.cpp
    src/encode.cpp
    src/tx_queue.cpp
    src/retained_store.cpp
    src/offline_queue.cpp
    src/inflight.cpp
    src/broker.cpp
    src/broker_policy.cpp
)

target_include_directories(mqtt_broker PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(mqtt_broker PUBLIC
    MQTT_BROKER_HOST_LE=1
)

if(MSVC)
    target_compile_options(mqtt_broker PRIVATE /W4)
else()
    target_compile_options(mqtt_broker PRIVATE -Wall -Wextra -Wpedantic)
endif()

# MSVC debug asserts → stderr (no modal dialogs blocking headless runs).
add_library(mqtt_test_support STATIC tests/test_host_env.cpp)

function(add_mqtt_test name)
    add_executable(${name} tests/${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(${name} PRIVATE mqtt_broker mqtt_test_support)
    if(MSVC)
        # Keep mqtt_test_support static init even when /OPT:REF would drop it.
        target_link_options(${name} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:mqtt_test_support>")
    endif()
endfunction()

add_mqtt_test(test_wire)
add_mqtt_test(test_varint)
add_mqtt_test(test_message_pool)
add_mqtt_test(test_topic_intern)
add_mqtt_test(test_topic_trie)
add_mqtt_test(test_qos_downgrade)
add_mqtt_test(test_properties)
add_mqtt_test(test_parser)
add_mqtt_test(test_tx_queue)
add_mqtt_test(test_broker)
add_mqtt_test(test_session)
add_mqtt_test(test_sparkplug_server)
add_executable(fuzz_parser tests/fuzz_parser.cpp)
target_include_directories(fuzz_parser PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(fuzz_parser PRIVATE mqtt_broker mqtt_test_support)
if(MSVC)
    target_link_options(fuzz_parser PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:mqtt_test_support>")
endif()

option(MQTT_BUILD_DEVICE_SUITE "Build live device conformance EXE (mqtt_device_suite)" ON)
if(MQTT_BUILD_DEVICE_SUITE)
    add_subdirectory(tools/device_suite)
endif()

enable_testing()
add_test(NAME test_wire COMMAND test_wire)
add_test(NAME test_varint COMMAND test_varint)
add_test(NAME test_message_pool COMMAND test_message_pool)
add_test(NAME test_topic_intern COMMAND test_topic_intern)
add_test(NAME test_topic_trie COMMAND test_topic_trie)
add_test(NAME test_qos_downgrade COMMAND test_qos_downgrade)
add_test(NAME test_properties COMMAND test_properties)
add_test(NAME test_parser COMMAND test_parser)
add_test(NAME test_tx_queue COMMAND test_tx_queue)
add_test(NAME test_broker COMMAND test_broker)
add_test(NAME test_session COMMAND test_session)
add_test(NAME test_sparkplug_server COMMAND test_sparkplug_server)
add_test(NAME fuzz_parser COMMAND fuzz_parser)