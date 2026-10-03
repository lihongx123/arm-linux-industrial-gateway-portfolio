add_library(mqmgateway_edge_core
    acquisition_scheduler.cpp
    alarm_event.cpp
    device_registry.cpp
    device_health.cpp
    diagnostics.cpp
    driver_manager.cpp
    gateway_core.cpp
    point_registry.cpp
    point_mapper.cpp
    telemetry_policy.cpp
    unified_message_v2.cpp
    watchdog.cpp
)

target_include_directories(mqmgateway_edge_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/iot_gateway
)

target_link_libraries(mqmgateway_edge_core PUBLIC Threads::Threads)
