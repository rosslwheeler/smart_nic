# Driver integration test
add_executable(driver_integration_test driver_integration_test.cpp)
target_link_libraries(driver_integration_test PRIVATE nic_driver::nic_driver)
target_compile_features(driver_integration_test PRIVATE cxx_std_20)
add_test(NAME driver_integration COMMAND driver_integration_test)

# RDMA two-driver loopback test
add_executable(rdma_loopback_test rdma_loopback_test.cpp)
target_link_libraries(rdma_loopback_test PRIVATE nic_driver::nic_driver)
target_compile_features(rdma_loopback_test PRIVATE cxx_std_20)
add_test(NAME rdma_loopback COMMAND rdma_loopback_test)

# Driver coverage test (edge cases for driver.cpp and packet_router.cpp)
add_executable(driver_coverage_test driver_coverage_test.cpp)
target_link_libraries(driver_coverage_test PRIVATE nic_driver::nic_driver)
target_compile_features(driver_coverage_test PRIVATE cxx_std_20)
add_test(NAME driver_coverage COMMAND driver_coverage_test)

# Common compile options for all driver test executables
set(DRIVER_TEST_TARGETS driver_integration_test rdma_loopback_test driver_coverage_test)
foreach(target ${DRIVER_TEST_TARGETS})
    # Keep asserts active in all build types (tests rely on assert() for validation)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    if (NIC_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -fext-numeric-literals)
    endif()
endforeach()
