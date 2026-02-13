# Driver library
add_library(nic_driver STATIC
    src/driver.cpp
    src/mmio_adapter.cpp
    src/packet_router.cpp
)

target_include_directories(nic_driver PUBLIC
    include
)

target_link_libraries(nic_driver PUBLIC nic::nic)

target_compile_features(nic_driver PUBLIC cxx_std_20)

if (MSVC)
    target_compile_options(nic_driver PRIVATE /W4)
else()
    target_compile_options(nic_driver PRIVATE -Wall -Wextra -Wpedantic)
    if (NIC_WARNINGS_AS_ERRORS)
        target_compile_options(nic_driver PRIVATE -Werror)
    endif()
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(nic_driver PRIVATE -fext-numeric-literals)
    endif()
endif()

# Create alias
add_library(nic_driver::nic_driver ALIAS nic_driver)

# Examples
if(NIC_BUILD_EXAMPLES)
    add_executable(echo_server examples/echo_server.cpp)
    target_link_libraries(echo_server PRIVATE nic_driver::nic_driver)
    target_compile_features(echo_server PRIVATE cxx_std_20)

    if (MSVC)
        target_compile_options(echo_server PRIVATE /W4)
    else()
        target_compile_options(echo_server PRIVATE -Wall -Wextra -Wpedantic)
        if (NIC_WARNINGS_AS_ERRORS)
            target_compile_options(echo_server PRIVATE -Werror)
        endif()
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(echo_server PRIVATE -fext-numeric-literals)
        endif()
    endif()
endif()
