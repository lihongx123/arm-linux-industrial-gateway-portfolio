add_library(mqmgateway_serial termios_rtu_transport.cpp serial_port.cpp)
target_include_directories(mqmgateway_serial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mqmgateway_serial PUBLIC Threads::Threads)

add_executable(mqmgateway_rtu_transport_tests
    ${PROJECT_SOURCE_DIR}/tests/serial/rtu_transport_tests.cpp
)
target_link_libraries(mqmgateway_rtu_transport_tests PRIVATE mqmgateway_serial util)

if (NOT WITHOUT_TESTS)
    add_test(NAME rtu_transport_stream_tests COMMAND mqmgateway_rtu_transport_tests)
endif()

install(TARGETS mqmgateway_serial DESTINATION lib)
install(TARGETS mqmgateway_rtu_transport_tests DESTINATION bin)
install(FILES termios_rtu_transport.hpp DESTINATION include/mqmgateway)
