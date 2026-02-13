# Device smoke test
add_executable(device_smoke_test device_smoke_test.cpp)
target_link_libraries(device_smoke_test PRIVATE nic)
add_test(NAME device_smoke_test COMMAND device_smoke_test)

# Config space test
add_executable(config_space_test config_space_test.cpp)
target_link_libraries(config_space_test PRIVATE nic)
add_test(NAME config_space_test COMMAND config_space_test)

# BAR test
add_executable(bar_test bar_test.cpp)
target_link_libraries(bar_test PRIVATE nic)
add_test(NAME bar_test COMMAND bar_test)

# Register test
add_executable(register_test register_test.cpp)
target_link_libraries(register_test PRIVATE nic)
add_test(NAME register_test COMMAND register_test)

add_executable(dma_host_test dma_host_test.cpp)
target_link_libraries(dma_host_test PRIVATE nic)
add_test(NAME dma_host_test COMMAND dma_host_test)

add_executable(tx_rx_test tx_rx_test.cpp)
target_link_libraries(tx_rx_test PRIVATE nic)
add_test(NAME tx_rx_test COMMAND tx_rx_test)

add_executable(queue_manager_rss_test queue_manager_rss_test.cpp)
target_link_libraries(queue_manager_rss_test PRIVATE nic)
add_test(NAME queue_manager_rss_test COMMAND queue_manager_rss_test)

add_executable(interrupt_dispatcher_test interrupt_dispatcher_test.cpp)
target_link_libraries(interrupt_dispatcher_test PRIVATE nic)
add_test(NAME interrupt_dispatcher_test COMMAND interrupt_dispatcher_test)

add_executable(virtual_function_test virtual_function_test.cpp)
target_link_libraries(virtual_function_test PRIVATE nic)
add_test(NAME virtual_function_test COMMAND virtual_function_test)

add_executable(pf_vf_manager_test pf_vf_manager_test.cpp)
target_link_libraries(pf_vf_manager_test PRIVATE nic)
add_test(NAME pf_vf_manager_test COMMAND pf_vf_manager_test)

add_executable(mailbox_test mailbox_test.cpp)
target_link_libraries(mailbox_test PRIVATE nic)
add_test(NAME mailbox_test COMMAND mailbox_test)

add_executable(vf_device_test vf_device_test.cpp)
target_link_libraries(vf_device_test PRIVATE nic)
add_test(NAME vf_device_test COMMAND vf_device_test)

add_executable(ptp_clock_test ptp_clock_test.cpp)
target_link_libraries(ptp_clock_test PRIVATE nic)
add_test(NAME ptp_clock_test COMMAND ptp_clock_test)

add_executable(ptp_timestamper_test ptp_timestamper_test.cpp)
target_link_libraries(ptp_timestamper_test PRIVATE nic)
add_test(NAME ptp_timestamper_test COMMAND ptp_timestamper_test)

add_executable(flow_control_test flow_control_test.cpp)
target_link_libraries(flow_control_test PRIVATE nic)
add_test(NAME flow_control_test COMMAND flow_control_test)

add_executable(telemetry_admin_test telemetry_admin_test.cpp)
target_link_libraries(telemetry_admin_test PRIVATE nic)
add_test(NAME telemetry_admin_test COMMAND telemetry_admin_test)

add_executable(validation_test validation_test.cpp)
target_link_libraries(validation_test PRIVATE nic)
add_test(NAME validation_test COMMAND validation_test)

add_executable(coverage_test coverage_test.cpp)
target_link_libraries(coverage_test PRIVATE nic)
add_test(NAME coverage_test COMMAND coverage_test)

add_executable(error_injector_test error_injector_test.cpp)
target_link_libraries(error_injector_test PRIVATE nic)
add_test(NAME error_injector_test COMMAND error_injector_test)

add_executable(device_test device_test.cpp)
target_link_libraries(device_test PRIVATE nic)
add_test(NAME device_test COMMAND device_test)

add_executable(stats_collector_test stats_collector_test.cpp)
target_link_libraries(stats_collector_test PRIVATE nic)
add_test(NAME stats_collector_test COMMAND stats_collector_test)

# Config space coverage test
add_executable(config_space_coverage_test config_space_coverage_test.cpp)
target_link_libraries(config_space_coverage_test PRIVATE nic)
add_test(NAME config_space_coverage_test COMMAND config_space_coverage_test)

# Register coverage test
add_executable(register_coverage_test register_coverage_test.cpp)
target_link_libraries(register_coverage_test PRIVATE nic)
add_test(NAME register_coverage_test COMMAND register_coverage_test)

# PCIe and register format tests
add_executable(pcie_formats_test pcie_formats_test.cpp)
target_link_libraries(pcie_formats_test PRIVATE nic)
add_test(NAME pcie_formats_test COMMAND pcie_formats_test)

add_executable(register_formats_test register_formats_test.cpp)
target_link_libraries(register_formats_test PRIVATE nic)
add_test(NAME register_formats_test COMMAND register_formats_test)

# RoCEv2 tests
add_executable(rocev2_memory_region_test rocev2/memory_region_test.cpp)
target_link_libraries(rocev2_memory_region_test PRIVATE nic)
add_test(NAME rocev2_memory_region_test COMMAND rocev2_memory_region_test)

add_executable(rocev2_queue_pair_test rocev2/queue_pair_test.cpp)
target_link_libraries(rocev2_queue_pair_test PRIVATE nic)
add_test(NAME rocev2_queue_pair_test COMMAND rocev2_queue_pair_test)

add_executable(rocev2_packet_test rocev2/packet_test.cpp)
target_link_libraries(rocev2_packet_test PRIVATE nic)
add_test(NAME rocev2_packet_test COMMAND rocev2_packet_test)

add_executable(rocev2_send_recv_test rocev2/send_recv_test.cpp)
target_link_libraries(rocev2_send_recv_test PRIVATE nic)
add_test(NAME rocev2_send_recv_test COMMAND rocev2_send_recv_test)

add_executable(rocev2_write_test rocev2/write_test.cpp)
target_link_libraries(rocev2_write_test PRIVATE nic)
add_test(NAME rocev2_write_test COMMAND rocev2_write_test)

add_executable(rocev2_read_test rocev2/read_test.cpp)
target_link_libraries(rocev2_read_test PRIVATE nic)
add_test(NAME rocev2_read_test COMMAND rocev2_read_test)

add_executable(rocev2_reliability_test rocev2/reliability_test.cpp)
target_link_libraries(rocev2_reliability_test PRIVATE nic)
add_test(NAME rocev2_reliability_test COMMAND rocev2_reliability_test)

add_executable(rocev2_congestion_test rocev2/congestion_test.cpp)
target_link_libraries(rocev2_congestion_test PRIVATE nic)
add_test(NAME rocev2_congestion_test COMMAND rocev2_congestion_test)

add_executable(rocev2_integration_test rocev2/integration_test.cpp)
target_link_libraries(rocev2_integration_test PRIVATE nic)
add_test(NAME rocev2_integration_test COMMAND rocev2_integration_test)

add_executable(rocev2_engine_coverage_test rocev2/engine_coverage_test.cpp)
target_link_libraries(rocev2_engine_coverage_test PRIVATE nic)
add_test(NAME rocev2_engine_coverage_test COMMAND rocev2_engine_coverage_test)

add_executable(rocev2_queue_pair_coverage_test rocev2/queue_pair_coverage_test.cpp)
target_link_libraries(rocev2_queue_pair_coverage_test PRIVATE nic)
add_test(NAME rocev2_queue_pair_coverage_test COMMAND rocev2_queue_pair_coverage_test)

add_executable(rocev2_pd_congestion_coverage_test rocev2/pd_congestion_coverage_test.cpp)
target_link_libraries(rocev2_pd_congestion_coverage_test PRIVATE nic)
add_test(NAME rocev2_pd_congestion_coverage_test COMMAND rocev2_pd_congestion_coverage_test)

# Tutorial tests (from docs/tutorial.md)
add_executable(tutorial_lesson1_test tutorial_lesson1_test.cpp)
target_link_libraries(tutorial_lesson1_test PRIVATE nic)
add_test(NAME tutorial_lesson1_test COMMAND tutorial_lesson1_test)

add_executable(tutorial_lesson2_test tutorial_lesson2_test.cpp)
target_link_libraries(tutorial_lesson2_test PRIVATE nic)
add_test(NAME tutorial_lesson2_test COMMAND tutorial_lesson2_test)

add_executable(tutorial_lesson3_test tutorial_lesson3_test.cpp)
target_link_libraries(tutorial_lesson3_test PRIVATE nic)
add_test(NAME tutorial_lesson3_test COMMAND tutorial_lesson3_test)

add_executable(tutorial_lesson4_test tutorial_lesson4_test.cpp)
target_link_libraries(tutorial_lesson4_test PRIVATE nic)
add_test(NAME tutorial_lesson4_test COMMAND tutorial_lesson4_test)

add_executable(tutorial_lesson5_test tutorial_lesson5_test.cpp)
target_link_libraries(tutorial_lesson5_test PRIVATE nic)
add_test(NAME tutorial_lesson5_test COMMAND tutorial_lesson5_test)

add_executable(tutorial_lesson6_test tutorial_lesson6_test.cpp)
target_link_libraries(tutorial_lesson6_test PRIVATE nic)
add_test(NAME tutorial_lesson6_test COMMAND tutorial_lesson6_test)

add_executable(tutorial_lesson7_test tutorial_lesson7_test.cpp)
target_link_libraries(tutorial_lesson7_test PRIVATE nic)
add_test(NAME tutorial_lesson7_test COMMAND tutorial_lesson7_test)

add_executable(tutorial_lesson8_test tutorial_lesson8_test.cpp)
target_link_libraries(tutorial_lesson8_test PRIVATE nic)
add_test(NAME tutorial_lesson8_test COMMAND tutorial_lesson8_test)

# Common compile options for all test executables
set(TEST_TARGETS device_smoke_test config_space_test config_space_coverage_test bar_test register_test register_coverage_test dma_host_test tx_rx_test queue_manager_rss_test interrupt_dispatcher_test virtual_function_test pf_vf_manager_test mailbox_test vf_device_test ptp_clock_test ptp_timestamper_test flow_control_test telemetry_admin_test validation_test coverage_test error_injector_test device_test stats_collector_test pcie_formats_test register_formats_test rocev2_memory_region_test rocev2_queue_pair_test rocev2_packet_test rocev2_send_recv_test rocev2_write_test rocev2_read_test rocev2_reliability_test rocev2_congestion_test rocev2_integration_test rocev2_engine_coverage_test rocev2_queue_pair_coverage_test rocev2_pd_congestion_coverage_test tutorial_lesson1_test tutorial_lesson2_test tutorial_lesson3_test tutorial_lesson4_test tutorial_lesson5_test tutorial_lesson6_test tutorial_lesson7_test tutorial_lesson8_test)
foreach(target ${TEST_TARGETS})
    # Keep asserts active in all build types (tests rely on assert() for validation)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -fext-numeric-literals)
    endif()
endforeach()
