add_library(iot_gateway_core
    can_socket.cpp
    command_router.cpp
    gateway.cpp
    unified_message.cpp
    southbound_reactor.cpp
    ../northbound/northbound_manager.cpp
    ../northbound/mqtt_northbound_adapter.cpp
    ../drivers/can_driver.cpp
    ../drivers/modbus_rtu_driver.cpp
    ../drivers/register_drivers.cpp
    ../drivers/tcp_driver.cpp
    ../drivers/modbus_tcp_driver.cpp
    ../drivers/mc_driver.cpp
    ../drivers/raw_uart_driver.cpp
    ../transport/tcp_transport.cpp
    ../board/board_driver.cpp
    ../board/linux_backends.cpp
)
if (WITH_OPCUA)
    target_sources(iot_gateway_core PRIVATE ../drivers/opcua_driver.cpp)
    target_include_directories(iot_gateway_core PUBLIC ${OPCUA_INCLUDE_DIR})
    target_link_libraries(iot_gateway_core PUBLIC ${OPCUA_LIBRARY})
    target_compile_definitions(iot_gateway_core PUBLIC MQM_WITH_OPCUA)
endif()
if (WITH_S7)
    target_sources(iot_gateway_core PRIVATE ../drivers/s7_driver.cpp ${SNAP7_CPP})
    target_include_directories(iot_gateway_core PUBLIC ${SNAP7_INCLUDE_DIR})
    target_link_libraries(iot_gateway_core PUBLIC ${SNAP7_LIBRARY})
    target_compile_definitions(iot_gateway_core PUBLIC MQM_WITH_S7)
endif()
target_include_directories(iot_gateway_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/drivers ${CMAKE_SOURCE_DIR}/src/board ${CMAKE_SOURCE_DIR}/src/northbound)
target_link_libraries(iot_gateway_core
    PUBLIC
    ${MOSQUITTO_LIBRARIES}
    Threads::Threads
    mqmgateway_serial
    mqmgateway_edge_core
)

add_executable(mqmgateway_iot main.cpp)
target_link_libraries(mqmgateway_iot PRIVATE iot_gateway_core)

install(TARGETS mqmgateway_iot DESTINATION bin)

add_executable(mqmgateway_rtu_driver_tests ../../tests/integration/rtu_driver_runtime_tests.cpp)
add_executable(mqmgateway_tcp_driver_tests ../../tests/integration/tcp_driver_runtime_tests.cpp)
add_executable(mqmgateway_mc_driver_tests ../../tests/integration/mc_driver_runtime_tests.cpp)
if (WITH_S7)
    add_executable(mqmgateway_s7_driver_tests ../../tests/integration/s7_driver_runtime_tests.cpp)
    target_link_libraries(mqmgateway_s7_driver_tests PRIVATE iot_gateway_core)
endif()
if (WITH_OPCUA)
    add_executable(mqmgateway_opcua_driver_tests ../../tests/integration/opcua_driver_runtime_tests.cpp)
    target_link_libraries(mqmgateway_opcua_driver_tests PRIVATE iot_gateway_core)
endif()
add_executable(mqmgateway_phase4_software_tests ../../tests/integration/phase4_software_tests.cpp)
add_executable(mqmgateway_phase5_mixed_tests ../../tests/integration/phase4_software_tests.cpp)
target_compile_definitions(mqmgateway_phase5_mixed_tests PRIVATE PHASE5_MIXED)
target_link_libraries(mqmgateway_phase4_software_tests PRIVATE iot_gateway_core)
target_link_libraries(mqmgateway_phase5_mixed_tests PRIVATE iot_gateway_core)
if (WITH_OPCUA AND WITH_S7)
    add_executable(mqmgateway_phase5_full_mixed_tests ../../tests/integration/phase4_software_tests.cpp)
    target_compile_definitions(mqmgateway_phase5_full_mixed_tests PRIVATE PHASE5_MIXED PHASE5_FULL_MIXED)
    target_link_libraries(mqmgateway_phase5_full_mixed_tests PRIVATE iot_gateway_core)
endif()
target_link_libraries(mqmgateway_tcp_driver_tests PRIVATE iot_gateway_core)
target_link_libraries(mqmgateway_mc_driver_tests PRIVATE iot_gateway_core)
target_link_libraries(mqmgateway_rtu_driver_tests PRIVATE iot_gateway_core)

if (NOT WITHOUT_TESTS)
    add_test(NAME phase4_software_tests COMMAND mqmgateway_phase4_software_tests)
    add_test(NAME phase5_mixed_tests COMMAND mqmgateway_phase5_mixed_tests)
    if (WITH_OPCUA AND WITH_S7)
        add_test(NAME phase5_full_mixed_tests COMMAND mqmgateway_phase5_full_mixed_tests)
    endif()
    add_test(NAME tcp_driver_runtime_tests COMMAND mqmgateway_tcp_driver_tests)
    add_test(NAME mc_driver_runtime_tests COMMAND mqmgateway_mc_driver_tests)
    if (WITH_S7)
        add_test(NAME s7_driver_runtime_tests COMMAND mqmgateway_s7_driver_tests)
    endif()
    if (WITH_OPCUA)
        add_test(NAME opcua_driver_runtime_tests COMMAND mqmgateway_opcua_driver_tests)
    endif()
    add_test(NAME rtu_driver_runtime_tests COMMAND mqmgateway_rtu_driver_tests)
    add_test(NAME iot_cloud_config_smoke
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/cloud/cloud_config_smoke_test.sh $<TARGET_FILE:mqmgateway_iot>)
endif()
