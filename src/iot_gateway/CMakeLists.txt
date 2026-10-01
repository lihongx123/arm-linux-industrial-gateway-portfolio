add_library(iot_gateway_core
    can_socket.cpp
    command_router.cpp
    gateway.cpp
    unified_message.cpp
    southbound_reactor.cpp
)

target_include_directories(iot_gateway_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(iot_gateway_core
    PUBLIC
    ${MOSQUITTO_LIBRARIES}
    Threads::Threads
    mqmgateway_serial
)

add_executable(mqmgateway_iot main.cpp)
target_link_libraries(mqmgateway_iot PRIVATE iot_gateway_core)

install(TARGETS mqmgateway_iot DESTINATION bin)

if (NOT WITHOUT_TESTS)
    add_test(NAME iot_cloud_config_smoke
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cloud/cloud_config_smoke_test.sh $<TARGET_FILE:mqmgateway_iot>)
endif()
