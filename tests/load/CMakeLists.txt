add_executable(mqmgateway_arm64_stress arm64_stress.cpp)
target_link_libraries(mqmgateway_arm64_stress PRIVATE ${MOSQUITTO_LIBRARIES} Threads::Threads)
install(TARGETS mqmgateway_arm64_stress DESTINATION bin)
