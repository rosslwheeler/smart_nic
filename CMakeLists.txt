cmake_minimum_required(VERSION 3.21)
project(nic_model LANGUAGES CXX)

option(NIC_ENABLE_TRACY "Enable Tracy profiling/tracing integration" ON)
option(NIC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(NIC_ENABLE_COVERAGE "Enable coverage instrumentation" OFF)
option(NIC_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(NIC_BUILD_DRIVER "Build driver library" ON)
option(NIC_BUILD_EXAMPLES "Build example applications" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(nic STATIC
    src/config_space.cpp
    src/device.cpp
    src/dma_types.cpp
    src/dma_engine.cpp
    src/descriptor_ring.cpp
    src/doorbell.cpp
    src/completion_queue.cpp
    src/interrupt_dispatcher.cpp
    src/msix.cpp
    src/queue_pair.cpp
    src/queue_manager.cpp
    src/rss.cpp
    src/checksum.cpp
    src/register.cpp
    src/simple_host_memory.cpp
    src/trace.cpp
    src/virtual_function.cpp
    src/pf_vf_manager.cpp
    src/mailbox.cpp
    src/vf_device.cpp
    src/ptp_clock.cpp
    src/ptp_timestamper.cpp
    src/flow_control.cpp
    src/stats_collector.cpp
    src/error_injector.cpp
    src/admin_queue.cpp
    src/packet_generator.cpp
    src/rocev2/protection_domain.cpp
    src/rocev2/memory_region.cpp
    src/rocev2/completion_queue.cpp
    src/rocev2/queue_pair.cpp
    src/rocev2/packet.cpp
    src/rocev2/send_recv.cpp
    src/rocev2/rdma_write.cpp
    src/rocev2/rdma_read.cpp
    src/rocev2/congestion.cpp
    src/rocev2/engine.cpp
)
target_include_directories(nic PUBLIC include)
target_compile_features(nic PUBLIC cxx_std_20)

if (MSVC)
    target_compile_options(nic PRIVATE /W4)
else()
    target_compile_options(nic PRIVATE -Wall -Wextra -Wpedantic)
    if (NIC_WARNINGS_AS_ERRORS)
        target_compile_options(nic PRIVATE -Werror)
    endif()
    # Tracy headers use hex color literals (e.g. 0x00ff00) that GCC -Wpedantic
    # rejects in C++20 mode. Allow them specifically.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(nic PRIVATE -fext-numeric-literals)
    endif()
endif()

if (NIC_ENABLE_COVERAGE)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_library(nic_coverage_config INTERFACE)
        target_compile_options(nic_coverage_config INTERFACE -O0 -g --coverage)
        target_link_options(nic_coverage_config INTERFACE --coverage)
        target_link_libraries(nic PUBLIC nic_coverage_config)
    else()
        message(WARNING "Coverage instrumentation is only supported for GCC/Clang.")
    endif()
endif()

if (NIC_ENABLE_ASAN)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_library(nic_asan_config INTERFACE)
        target_compile_options(nic_asan_config INTERFACE
            -fsanitize=address
            -g
            -O1
            -fno-omit-frame-pointer
        )
        target_link_options(nic_asan_config INTERFACE -fsanitize=address)
        target_link_libraries(nic PUBLIC nic_asan_config)
    else()
        message(WARNING "AddressSanitizer is only supported for GCC/Clang.")
    endif()
endif()

# Tracy integration - always include headers, optionally enable profiling
set(TRACY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third-party/tracy)
if (EXISTS ${TRACY_ROOT}/public/tracy/Tracy.hpp)
    target_include_directories(nic SYSTEM PUBLIC ${TRACY_ROOT}/public ${TRACY_ROOT}/public/tracy)
    if (NIC_ENABLE_TRACY)
        add_subdirectory(${TRACY_ROOT} third-party/tracy_build)
        # Suppress warnings in Tracy's own source (third-party code)
        if (TARGET TracyClient)
            target_compile_options(TracyClient PRIVATE -w)
        endif()
        target_link_libraries(nic PUBLIC Tracy::TracyClient)
        target_compile_definitions(nic PUBLIC TRACY_ENABLE)
    endif()
else()
    message(FATAL_ERROR "Tracy submodule not found at third-party/tracy. Initialize the submodule.")
endif()

add_library(nic::nic ALIAS nic)

# bit_fields library (header-only, SYSTEM suppresses warnings from third-party headers)
add_subdirectory(libs/bit_fields)
target_link_libraries(nic PUBLIC bit_fields::bit_fields)
get_target_property(_bf_inc bit_fields INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(nic SYSTEM PUBLIC ${_bf_inc})

# Driver library (optional)
if(NIC_BUILD_DRIVER)
    add_subdirectory(driver)
endif()

enable_testing()
add_subdirectory(tests)

# Driver tests (optional)
if(NIC_BUILD_DRIVER)
    add_subdirectory(tests/driver)
endif()
